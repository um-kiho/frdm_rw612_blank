/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BH1750 I/O adapter built on top of the shared i2c_bus front-end so that
 * BH1750 transfers are serialised with every other device sharing the bus
 * (AMG8833, future I2C peripherals).
 *
 * The legacy BH1750_I2C_INSTANCE / BH1750_I2C_BAUDRATE_HZ macros in the
 * companion header are now ignored - bus configuration lives in i2c_bus.h
 * (APP_I2C_BUS_INSTANCE / APP_I2C_BUS_BAUDRATE_HZ).
 */

#include "bh1750_io_nxp_i2c.h"
#include "i2c_bus.h"

#include <zephyr/kernel.h>

static int nxp_i2c_write(uint8_t addr_7b, const uint8_t *data, size_t len,
                         void *ctx)
{
    (void)ctx;
    return (i2c_bus_write(addr_7b, data, len) == 0) ? 0 : -1;
}

static int nxp_i2c_read(uint8_t addr_7b, uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    return (i2c_bus_read(addr_7b, data, len) == 0) ? 0 : -1;
}

static void rtos_delay_ms(uint32_t ms)
{
    k_sleep(K_MSEC(ms));
}

int bh1750_io_nxp_i2c_init(bh1750_io_t *out_io)
{
    if (out_io == NULL) return BH1750_ERR_PARAM;
    if (i2c_bus_init() != 0) return BH1750_ERR_IO;

    out_io->write    = nxp_i2c_write;
    out_io->read     = nxp_i2c_read;
    out_io->delay_ms = rtos_delay_ms;
    out_io->ctx      = NULL;
    return BH1750_OK;
}
