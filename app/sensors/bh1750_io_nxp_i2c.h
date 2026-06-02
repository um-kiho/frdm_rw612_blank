/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * FRDM-RW612 FlexComm I2C adapter for the BH1750 driver.
 *
 * Defaults follow develop/frdmrw612_io_interface_spec.md:
 *   - bus    : I2C2 (FLEXCOMM2)
 *   - speed  : 100 kHz bring-up (Fast-mode 400 kHz once signal integrity OK)
 *   - sensor : BH1750FVI x 2 sharing the bus (addresses 0x23 + 0x5C)
 *
 * The adapter exposes one bh1750_io_t shared by both sensor instances; the
 * driver targets each chip by its 7-bit address.
 */

#ifndef BH1750_IO_NXP_I2C_H
#define BH1750_IO_NXP_I2C_H

#include <stdint.h>
#include "bh1750.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The adapter is layered on top of i2c_bus.{h,c}; configure the bus
 * instance / baud rate there via APP_I2C_BUS_INSTANCE / APP_I2C_BUS_BAUDRATE_HZ.
 * The two legacy macros below are kept for backward source compatibility but
 * are NOT consulted by the implementation any more. */
#ifndef BH1750_I2C_INSTANCE
#define BH1750_I2C_INSTANCE       2u
#endif
#ifndef BH1750_I2C_BAUDRATE_HZ
#define BH1750_I2C_BAUDRATE_HZ    400000u
#endif

/* One-shot initialisation of the I2C master and the io pair. May be called
 * more than once; subsequent calls only re-fill out_io. */
int bh1750_io_nxp_i2c_init(bh1750_io_t *out_io);

#ifdef __cplusplus
}
#endif

#endif /* BH1750_IO_NXP_I2C_H */
