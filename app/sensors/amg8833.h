/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Panasonic AMG8833 (Grid-EYE) 8x8 infrared thermopile array driver.
 *
 * Datasheet quick-reference:
 *   - 7-bit I2C addresses: 0x68 (AD_SELECT=GND) / 0x69 (AD_SELECT=VDD)
 *   - I2C up to 400 kHz, no clock stretching concerns at <=400 kHz
 *   - Registers used here:
 *       0x00 PCTL  : 0x00=Normal, 0x10=Sleep, 0x20/0x21=Stand-by
 *       0x01 RST   : 0x3F=Initial reset, 0x30=Flag reset
 *       0x02 FPSC  : bit0 0=10 fps, 1=1 fps
 *       0x03 INTC  : interrupt control (we disable: 0x00)
 *       0x0E/0x0F  : thermistor TTHL/TTHH (signed 12-bit, 0.0625 C/LSB)
 *       0x80..0xFF : 64 pixels x 2 bytes (signed 12-bit, 0.25  C/LSB)
 *
 * Pixel encoding: LSB+MSB pair, the value is a 12-bit two's complement
 * magnitude that this driver sign-extends to int16. The raw value carries
 * 0.25 C per LSB. Helpers convert to centi-degrees Celsius (0.01 C units).
 */

#ifndef AMG8833_H
#define AMG8833_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMG8833_ADDR_LOW          0x68u
#define AMG8833_ADDR_HIGH         0x69u

#define AMG8833_PIXELS            64
#define AMG8833_FRAME_BYTES       (AMG8833_PIXELS * 2)

#define AMG8833_REG_PCTL          0x00u
#define AMG8833_REG_RST           0x01u
#define AMG8833_REG_FPSC          0x02u
#define AMG8833_REG_INTC          0x03u
#define AMG8833_REG_STAT          0x04u
#define AMG8833_REG_SCLR          0x05u
#define AMG8833_REG_AVE           0x07u
#define AMG8833_REG_TTHL          0x0Eu
#define AMG8833_REG_TTHH          0x0Fu
#define AMG8833_REG_T01L          0x80u

#define AMG8833_PCTL_NORMAL       0x00u
#define AMG8833_PCTL_SLEEP        0x10u
#define AMG8833_RST_FLAG          0x30u
#define AMG8833_RST_INITIAL       0x3Fu
#define AMG8833_FPSC_10FPS        0x00u
#define AMG8833_FPSC_1FPS         0x01u
#define AMG8833_INTC_DISABLE      0x00u

#define AMG8833_OK                ( 0)
#define AMG8833_ERR_PARAM         (-1)
#define AMG8833_ERR_IO            (-2)

typedef struct amg8833 {
    uint8_t  addr;                       /* 7-bit                             */
    uint8_t  fps;                        /* AMG8833_FPSC_10FPS or 1FPS        */
    int16_t  pixels_q2[AMG8833_PIXELS];  /* signed; LSB = 0.25 C              */
    int16_t  thermistor_q4;              /* signed; LSB = 0.0625 C            */
} amg8833_t;

/* Initialise driver state. Touches the bus to bring the chip out of reset
 * and put it into Normal mode + 10 fps. */
int  amg8833_init      (amg8833_t *dev, uint8_t addr_7b);

/* Selects 10 fps or 1 fps. Pass AMG8833_FPSC_10FPS / _1FPS. */
int  amg8833_set_fps   (amg8833_t *dev, uint8_t fpsc);

/* Reads all 64 pixels into dev->pixels_q2. */
int  amg8833_read_frame(amg8833_t *dev);

/* Reads the thermistor into dev->thermistor_q4. */
int  amg8833_read_thermistor(amg8833_t *dev);

/* Convenience: 0.25 C signed -> 0.01 C signed (centi-degrees). */
static inline int16_t amg8833_pix_q2_to_c_x100(int16_t q2)
{
    return (int16_t)((int32_t)q2 * 25);
}

#ifdef __cplusplus
}
#endif

#endif /* AMG8833_H */
