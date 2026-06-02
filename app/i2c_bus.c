/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "i2c_bus.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/device.h>

#if   (APP_I2C_BUS_INSTANCE == 0u)
  #define I2C_NODE DT_NODELABEL(flexcomm0)
#elif (APP_I2C_BUS_INSTANCE == 1u)
  #define I2C_NODE DT_NODELABEL(flexcomm1)
#elif (APP_I2C_BUS_INSTANCE == 2u)
  #define I2C_NODE DT_NODELABEL(flexcomm2)
#elif (APP_I2C_BUS_INSTANCE == 3u)
  #define I2C_NODE DT_NODELABEL(flexcomm3)
#else
  #error "APP_I2C_BUS_INSTANCE must be 0..3"
#endif

/* Resolved at link time; speed/pinctrl configured in the device tree. */
static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_NODE);

/* Statically initialised — no runtime k_mutex_init() call needed. */
static K_MUTEX_DEFINE(s_lock);

int i2c_bus_init(void)
{
    if (!device_is_ready(i2c_dev)) {
        return -ENODEV;
    }
    return 0;
}

int i2c_bus_write(uint8_t addr_7b, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0u) return -EINVAL;

    int rc;
    k_mutex_lock(&s_lock, K_FOREVER);
    rc = i2c_write(i2c_dev, data, len, addr_7b);
    k_mutex_unlock(&s_lock);
    return rc;
}

int i2c_bus_read(uint8_t addr_7b, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0u) return -EINVAL;

    int rc;
    k_mutex_lock(&s_lock, K_FOREVER);
    rc = i2c_read(i2c_dev, data, len, addr_7b);
    k_mutex_unlock(&s_lock);
    return rc;
}

int i2c_bus_probe(uint8_t addr_7b)
{
    uint8_t dummy;
    int rc;
    k_mutex_lock(&s_lock, K_FOREVER);
    rc = i2c_read(i2c_dev, &dummy, 1, addr_7b);
    k_mutex_unlock(&s_lock);
    return (rc == 0) ? 0 : -ENODEV;
}

void i2c_bus_force_unlock(void)
{
    k_mutex_init(&s_lock);
}

int i2c_bus_write_read(uint8_t addr_7b,
                       const uint8_t *tx, size_t tx_len,
                       uint8_t       *rx, size_t rx_len)
{
    if (tx == NULL || rx == NULL)     return -EINVAL;
    if (tx_len == 0u || rx_len == 0u) return -EINVAL;

    int rc;
    k_mutex_lock(&s_lock, K_FOREVER);
    rc = i2c_write_read(i2c_dev, addr_7b, tx, tx_len, rx, rx_len);
    k_mutex_unlock(&s_lock);
    return rc;
}
