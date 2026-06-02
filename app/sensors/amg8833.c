/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "amg8833.h"
#include "i2c_bus.h"

#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(amg8833, LOG_LEVEL_INF);

static int wr_reg(amg8833_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return (i2c_bus_write(dev->addr, buf, 2) == 0) ? AMG8833_OK : AMG8833_ERR_IO;
}

static int16_t sign_extend_12(uint16_t raw)
{
    /* Two's complement 12-bit magnitude in the low nibble+byte of a 16-bit
     * word. Bit 11 = sign. */
    if (raw & 0x0800u) {
        return (int16_t)(raw | 0xF000u);
    }
    return (int16_t)(raw & 0x0FFFu);
}

int amg8833_init(amg8833_t *dev, uint8_t addr_7b)
{
    if (dev == NULL) return AMG8833_ERR_PARAM;
    if (addr_7b != AMG8833_ADDR_LOW && addr_7b != AMG8833_ADDR_HIGH) {
        return AMG8833_ERR_PARAM;
    }
    memset(dev, 0, sizeof(*dev));
    dev->addr = addr_7b;
    dev->fps  = AMG8833_FPSC_10FPS;

    if (i2c_bus_init() != 0) return AMG8833_ERR_IO;

    /* Boot sequence per datasheet. Each register write is short so the
     * shared bus mutex is only held momentarily; the inter-step delays use
     * k_sleep so other tasks can run. */
    int rc = wr_reg(dev, AMG8833_REG_PCTL, AMG8833_PCTL_NORMAL);
    if (rc != AMG8833_OK) return rc;
    k_sleep(K_MSEC(50));

    rc = wr_reg(dev, AMG8833_REG_RST, AMG8833_RST_INITIAL);
    if (rc != AMG8833_OK) return rc;
    k_sleep(K_MSEC(2));

    rc = wr_reg(dev, AMG8833_REG_FPSC, dev->fps);
    if (rc != AMG8833_OK) return rc;

    rc = wr_reg(dev, AMG8833_REG_INTC, AMG8833_INTC_DISABLE);
    if (rc != AMG8833_OK) return rc;

    /* First valid frame appears after ~100 ms (one frame at 10 fps). */
    k_sleep(K_MSEC(100));

    LOG_INF("init @0x%02X OK (10 fps)", (unsigned)addr_7b);
    return AMG8833_OK;
}

int amg8833_set_fps(amg8833_t *dev, uint8_t fpsc)
{
    if (dev == NULL) return AMG8833_ERR_PARAM;
    if (fpsc != AMG8833_FPSC_1FPS && fpsc != AMG8833_FPSC_10FPS) {
        return AMG8833_ERR_PARAM;
    }
    int rc = wr_reg(dev, AMG8833_REG_FPSC, fpsc);
    if (rc == AMG8833_OK) {
        dev->fps = fpsc;
    }
    return rc;
}

int amg8833_read_frame(amg8833_t *dev)
{
    if (dev == NULL) return AMG8833_ERR_PARAM;

    uint8_t reg = AMG8833_REG_T01L;
    uint8_t raw[AMG8833_FRAME_BYTES];

    if (i2c_bus_write_read(dev->addr, &reg, 1u, raw, sizeof(raw)) != 0) {
        return AMG8833_ERR_IO;
    }

    for (int i = 0; i < AMG8833_PIXELS; ++i) {
        uint16_t lo = raw[i * 2 + 0];
        uint16_t hi = raw[i * 2 + 1];
        dev->pixels_q2[i] = sign_extend_12((uint16_t)((hi << 8) | lo));
    }
    return AMG8833_OK;
}

int amg8833_read_thermistor(amg8833_t *dev)
{
    if (dev == NULL) return AMG8833_ERR_PARAM;

    uint8_t reg = AMG8833_REG_TTHL;
    uint8_t raw[2];

    if (i2c_bus_write_read(dev->addr, &reg, 1u, raw, sizeof(raw)) != 0) {
        return AMG8833_ERR_IO;
    }
    uint16_t r = (uint16_t)((raw[1] << 8) | raw[0]);
    /* Datasheet: 11-bit magnitude + sign bit 11. 0.0625 C/LSB. */
    if (r & 0x0800u) {
        dev->thermistor_q4 = -(int16_t)(r & 0x07FFu);
    } else {
        dev->thermistor_q4 =  (int16_t)(r & 0x07FFu);
    }
    return AMG8833_OK;
}
