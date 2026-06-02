/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "bh1750.h"

#include <stdint.h>
#include <stddef.h>

#ifndef BH1750_CONV_TIME_H_MS
#define BH1750_CONV_TIME_H_MS   180u   /* H modes typ 120 ms, max ~180 ms */
#endif
#ifndef BH1750_CONV_TIME_L_MS
#define BH1750_CONV_TIME_L_MS   24u    /* L mode  typ 16  ms, max ~24  ms */
#endif

/* Crude fallback when the adapter does not provide a delay hook. The exact
 * timing is not critical here - the loop only has to gate the next I2C read
 * for tens of milliseconds, well above the host's I2C transaction time. */
static void busy_wait_ms(uint32_t ms)
{
    volatile uint32_t loops = ms * 24000u;     /* ~tuned for >100 MHz cores */
    while (loops--) { __asm volatile ("nop"); }
}

static void bh1750_delay_ms(const bh1750_t *dev, uint32_t ms)
{
    if (dev->io.delay_ms != NULL) {
        dev->io.delay_ms(ms);
    } else {
        busy_wait_ms(ms);
    }
}

static uint8_t mode_to_opcode(bh1750_mode_t m)
{
    switch (m) {
    case BH1750_MODE_CONT_H:  return BH1750_OP_CONT_H;
    case BH1750_MODE_CONT_H2: return BH1750_OP_CONT_H2;
    case BH1750_MODE_CONT_L:  return BH1750_OP_CONT_L;
    case BH1750_MODE_ONE_H:   return BH1750_OP_ONE_H;
    case BH1750_MODE_ONE_H2:  return BH1750_OP_ONE_H2;
    case BH1750_MODE_ONE_L:   return BH1750_OP_ONE_L;
    default:                  return BH1750_OP_CONT_H;
    }
}

static uint32_t mode_to_conv_ms(bh1750_mode_t m)
{
    switch (m) {
    case BH1750_MODE_CONT_L:
    case BH1750_MODE_ONE_L:
        return BH1750_CONV_TIME_L_MS;
    default:
        return BH1750_CONV_TIME_H_MS;
    }
}

static int is_one_shot(bh1750_mode_t m)
{
    return (m == BH1750_MODE_ONE_H ||
            m == BH1750_MODE_ONE_H2 ||
            m == BH1750_MODE_ONE_L) ? 1 : 0;
}

static int bh1750_write_op(bh1750_t *dev, uint8_t op)
{
    if (dev->io.write == NULL) return BH1750_ERR_IO;
    if (dev->io.write(dev->addr, &op, 1, dev->io.ctx) != 0) {
        return BH1750_ERR_IO;
    }
    return BH1750_OK;
}

int bh1750_init(bh1750_t *dev, const bh1750_io_t *io, uint8_t addr_7b)
{
    if (dev == NULL || io == NULL) return BH1750_ERR_PARAM;
    if (io->write == NULL || io->read == NULL) return BH1750_ERR_PARAM;
    if (addr_7b != BH1750_ADDR_LOW && addr_7b != BH1750_ADDR_HIGH) {
        return BH1750_ERR_PARAM;
    }
    dev->io       = *io;
    dev->addr     = addr_7b;
    dev->mode     = BH1750_MODE_CONT_H;
    dev->last_raw = 0u;
    return BH1750_OK;
}

int bh1750_power_on (bh1750_t *dev) { return dev ? bh1750_write_op(dev, BH1750_OP_POWER_ON ) : BH1750_ERR_PARAM; }
int bh1750_power_off(bh1750_t *dev) { return dev ? bh1750_write_op(dev, BH1750_OP_POWER_DOWN) : BH1750_ERR_PARAM; }

int bh1750_reset(bh1750_t *dev)
{
    int rc;
    if (dev == NULL) return BH1750_ERR_PARAM;
    rc = bh1750_power_on(dev);
    if (rc != BH1750_OK) return rc;
    return bh1750_write_op(dev, BH1750_OP_RESET);
}

int bh1750_set_mode(bh1750_t *dev, bh1750_mode_t mode)
{
    int rc;
    if (dev == NULL) return BH1750_ERR_PARAM;
    rc = bh1750_power_on(dev);
    if (rc != BH1750_OK) return rc;
    rc = bh1750_write_op(dev, mode_to_opcode(mode));
    if (rc != BH1750_OK) return rc;
    dev->mode = mode;
    return BH1750_OK;
}

int bh1750_read_raw(bh1750_t *dev, uint16_t *raw_out)
{
    if (dev == NULL || raw_out == NULL) return BH1750_ERR_PARAM;
    if (dev->io.read == NULL)           return BH1750_ERR_IO;

    /* One-shot modes have to be re-armed before every read. */
    if (is_one_shot(dev->mode)) {
        int rc = bh1750_write_op(dev, mode_to_opcode(dev->mode));
        if (rc != BH1750_OK) return rc;
    }

    bh1750_delay_ms(dev, mode_to_conv_ms(dev->mode));

    uint8_t buf[2] = {0, 0};
    if (dev->io.read(dev->addr, buf, sizeof(buf), dev->io.ctx) != 0) {
        return BH1750_ERR_IO;
    }
    uint16_t raw = ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
    dev->last_raw = raw;
    *raw_out      = raw;
    return BH1750_OK;
}

int bh1750_read_lux(bh1750_t *dev, float *lux_out)
{
    uint16_t raw;
    int rc;
    if (dev == NULL || lux_out == NULL) return BH1750_ERR_PARAM;
    rc = bh1750_read_raw(dev, &raw);
    if (rc != BH1750_OK) return rc;

    float scale = (dev->mode == BH1750_MODE_CONT_H2 ||
                   dev->mode == BH1750_MODE_ONE_H2) ? 2.4f : 1.2f;
    *lux_out = (float)raw / scale;
    return BH1750_OK;
}

int bh1750_read_lux_x100(bh1750_t *dev, uint32_t *lux_x100_out)
{
    uint16_t raw;
    int rc;
    if (dev == NULL || lux_x100_out == NULL) return BH1750_ERR_PARAM;
    rc = bh1750_read_raw(dev, &raw);
    if (rc != BH1750_OK) return rc;

    /* H2 mode: raw / 2.4 * 100 = raw * 1000 / 24
     * other  : raw / 1.2 * 100 = raw * 1000 / 12
     * Compute via 32-bit integers to avoid float in callers that need not
     * pay for the FPU. */
    uint32_t numer = (uint32_t)raw * 1000u;
    uint32_t denom = (dev->mode == BH1750_MODE_CONT_H2 ||
                      dev->mode == BH1750_MODE_ONE_H2) ? 24u : 12u;
    *lux_x100_out = numer / denom;
    return BH1750_OK;
}
