/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BH1750FVI ambient light sensor driver.
 *
 * Datasheet reference (Rohm BH1750FVI):
 *   - 7-bit I2C addresses:
 *       0x23 when ADDR pin is tied LOW
 *       0x5C when ADDR pin is tied HIGH
 *   - Range: 1 .. 65535 lx (H mode), 0.5 lx resolution in H2 mode
 *   - Opcodes:
 *       0x00 Power Down
 *       0x01 Power On
 *       0x07 Reset (must be in Power On)
 *       0x10 Continuously H-Resolution Mode      (1   lx,  ~120 ms)
 *       0x11 Continuously H-Resolution Mode 2    (0.5 lx,  ~120 ms)
 *       0x13 Continuously L-Resolution Mode      (4   lx,   ~16 ms)
 *       0x20 One-time     H-Resolution Mode
 *       0x21 One-time     H-Resolution Mode 2
 *       0x23 One-time     L-Resolution Mode
 *   - Conversion:  lx = raw / 1.2                (H or L mode)
 *                  lx = raw / 1.2 / 2  = raw / 2.4   (H2 mode)
 *
 * The driver itself is I/O-agnostic; the caller provides a bh1750_io_t pair
 * of write/read function pointers. A FRDM-RW612 FlexComm I2C adapter is
 * available in bh1750_io_nxp_i2c.{h,c}.
 */

#ifndef BH1750_H
#define BH1750_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BH1750_ADDR_LOW        0x23u   /* ADDR pin tied low  (default) */
#define BH1750_ADDR_HIGH       0x5Cu   /* ADDR pin tied high           */

#define BH1750_OP_POWER_DOWN   0x00u
#define BH1750_OP_POWER_ON     0x01u
#define BH1750_OP_RESET        0x07u
#define BH1750_OP_CONT_H       0x10u
#define BH1750_OP_CONT_H2      0x11u
#define BH1750_OP_CONT_L       0x13u
#define BH1750_OP_ONE_H        0x20u
#define BH1750_OP_ONE_H2       0x21u
#define BH1750_OP_ONE_L        0x23u

typedef enum bh1750_mode {
    BH1750_MODE_CONT_H = 0,    /* 1   lx, 120 ms */
    BH1750_MODE_CONT_H2,       /* 0.5 lx, 120 ms */
    BH1750_MODE_CONT_L,        /* 4   lx,  16 ms */
    BH1750_MODE_ONE_H,
    BH1750_MODE_ONE_H2,
    BH1750_MODE_ONE_L,
} bh1750_mode_t;

/* Driver-side return codes. Negative = error. */
#define BH1750_OK              ( 0)
#define BH1750_ERR_PARAM       (-1)
#define BH1750_ERR_IO          (-2)
#define BH1750_ERR_TIMEOUT     (-3)
#define BH1750_ERR_STATE       (-4)

/*
 * I/O callbacks. Both must be implemented by the bus adapter. Each transfer
 * targets a 7-bit BH1750 I2C address. Return 0 on success or any negative
 * value on transport failure (it is mapped to BH1750_ERR_IO).
 */
typedef int (*bh1750_i2c_write_fn)(uint8_t addr_7b,
                                   const uint8_t *data, size_t len,
                                   void *ctx);
typedef int (*bh1750_i2c_read_fn) (uint8_t addr_7b,
                                         uint8_t *data, size_t len,
                                   void *ctx);
typedef void (*bh1750_delay_ms_fn)(uint32_t ms);

typedef struct bh1750_io {
    bh1750_i2c_write_fn write;
    bh1750_i2c_read_fn  read;
    bh1750_delay_ms_fn  delay_ms;     /* may be NULL; falls back to busy loop */
    void               *ctx;
} bh1750_io_t;

typedef struct bh1750 {
    bh1750_io_t     io;
    uint8_t         addr;             /* 7-bit                            */
    bh1750_mode_t   mode;             /* current measurement mode         */
    uint16_t        last_raw;         /* most recent raw 16-bit reading   */
} bh1750_t;

/* Initialise driver state. Does not touch the bus. */
int bh1750_init     (bh1750_t *dev, const bh1750_io_t *io, uint8_t addr_7b);

/* Bus operations. */
int bh1750_power_on (bh1750_t *dev);
int bh1750_power_off(bh1750_t *dev);
int bh1750_reset    (bh1750_t *dev);

/* Select a measurement mode. Implicitly powers the device on. */
int bh1750_set_mode (bh1750_t *dev, bh1750_mode_t mode);

/* Read one sample. In continuous modes the caller may poll faster than the
 * sensor's update rate; in one-shot modes the driver waits for the typical
 * conversion time of the chosen mode before reading. */
int bh1750_read_raw (bh1750_t *dev, uint16_t *raw_out);
int bh1750_read_lux (bh1750_t *dev, float    *lux_out);

/* Fixed-point variant: returns lux * 100 (i.e. units of 0.01 lx). */
int bh1750_read_lux_x100(bh1750_t *dev, uint32_t *lux_x100_out);

#ifdef __cplusplus
}
#endif

#endif /* BH1750_H */
