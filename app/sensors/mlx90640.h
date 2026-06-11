/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Melexis MLX90640 32x24 (768 pixel) far-infrared thermal array driver.
 *
 * Interface: I2C only (the MLX90640 has no other bus). Goes through the shared
 * app/i2c_bus.{h,c} front-end so it is serialised with the other I2C sensors.
 *
 * Datasheet quick-reference:
 *   - 7-bit I2C address: 0x33 (default)
 *   - I2C up to 1 MHz (FM+); 400 kHz used on this board's shared bus
 *   - 16-bit register/RAM addressing, 16-bit big-endian words
 *   - EEPROM (calibration)   : 0x2400..0x273F (832 words)
 *   - RAM (frame + aux)      : 0x0400..0x073F (832 words)
 *   - Status register        : 0x8000  (bit3 = new data ready, bit0 = subpage)
 *   - Control register 1     : 0x800D  (refresh rate, resolution, mode)
 *
 * Temperature pipeline (per Melexis MLX90640 API, adapted here):
 *   dump_ee -> extract_parameters  (once, after init)
 *   get_frame -> calculate_to       (each frame -> dev->To[768] in deg C)
 *
 * NOTE: the calibration / To math below is adapted from the Melexis
 *       MLX90640 driver API (Apache-2.0). Validate against a known target on
 *       first bring-up.
 */

#ifndef MLX90640_H
#define MLX90640_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MLX90640_ADDR_DEFAULT     0x33u

#define MLX90640_PIXELS           768            /* 32 columns x 24 rows       */
#define MLX90640_COLS             32
#define MLX90640_ROWS             24
#define MLX90640_EE_WORDS         832            /* EEPROM dump size           */
#define MLX90640_FRAME_WORDS      834            /* 832 RAM + ctrl1 + subpage  */

/* Register addresses (16-bit) */
#define MLX90640_REG_STATUS       0x8000u
#define MLX90640_REG_CONTROL1     0x800Du
#define MLX90640_RAM_BASE         0x0400u
#define MLX90640_EE_BASE          0x2400u

/* Refresh rate codes for mlx90640_init() / set_refresh_rate() */
#define MLX90640_RR_0_5HZ         0x00u
#define MLX90640_RR_1HZ           0x01u
#define MLX90640_RR_2HZ           0x02u
#define MLX90640_RR_4HZ           0x03u
#define MLX90640_RR_8HZ           0x04u
#define MLX90640_RR_16HZ          0x05u
#define MLX90640_RR_32HZ          0x06u
#define MLX90640_RR_64HZ          0x07u

/* ADC resolution codes for set_resolution() (16/17/18/19-bit) */
#define MLX90640_RES_16BIT        0x00u
#define MLX90640_RES_17BIT        0x01u
#define MLX90640_RES_18BIT        0x02u
#define MLX90640_RES_19BIT        0x03u

#define MLX90640_OK               ( 0)
#define MLX90640_ERR_PARAM        (-1)
#define MLX90640_ERR_IO           (-2)
#define MLX90640_ERR_VERIFY       (-3)   /* register write read-back mismatch  */
#define MLX90640_ERR_TIMEOUT      (-4)   /* data-ready never asserted          */

typedef struct mlx90640 {
    uint8_t  addr;                 /* 7-bit I2C address                        */

    /* --- calibration parameters extracted from EEPROM --- */
    int16_t  kVdd;
    int16_t  vdd25;
    float    KvPTAT;
    float    KtPTAT;
    uint16_t vPTAT25;
    float    alphaPTAT;
    int16_t  gainEE;
    float    tgc;
    float    cpKv;
    float    cpKta;
    uint8_t  resolutionEE;
    uint8_t  calibrationModeEE;
    float    KsTa;
    float    ksTo[5];
    int16_t  ct[5];
    uint16_t alpha[MLX90640_PIXELS];
    uint8_t  alphaScale;
    int16_t  offset[MLX90640_PIXELS];
    int8_t   kta[MLX90640_PIXELS];
    uint8_t  ktaScale;
    int8_t   kv[MLX90640_PIXELS];
    uint8_t  kvScale;
    float    cpAlpha[2];
    int16_t  cpOffset[2];
    float    ilChessC[3];
    uint16_t brokenPixels[5];
    uint16_t outlierPixels[5];

    /* --- working buffers --- */
    uint16_t eeData[MLX90640_EE_WORDS];     /* raw EEPROM dump                 */
    uint16_t frame[MLX90640_FRAME_WORDS];   /* last raw frame                  */
    float    To[MLX90640_PIXELS];           /* last object temperatures (degC) */
} mlx90640_t;

/* One-shot init: brings up i2c_bus, sets refresh rate, dumps EEPROM and
 * extracts the calibration parameters. After this, call get_frame() +
 * calculate_to() repeatedly. */
int   mlx90640_init             (mlx90640_t *dev, uint8_t addr_7b, uint8_t refresh_rate);

/* Read the 832-word EEPROM into dev->eeData. */
int   mlx90640_dump_ee          (mlx90640_t *dev);

/* Parse dev->eeData into the calibration parameter fields. */
int   mlx90640_extract_parameters(mlx90640_t *dev);

/* Wait for data-ready, read one sub-page frame into dev->frame. Returns the
 * sub-page number (0/1) on success, or a negative MLX90640_ERR_* code. A full
 * image needs both sub-pages (call twice). */
int   mlx90640_get_frame        (mlx90640_t *dev);

/* Supply voltage / ambient (sensor) temperature from the last frame. */
float mlx90640_get_vdd          (mlx90640_t *dev);
float mlx90640_get_ta           (mlx90640_t *dev);

/* Compute per-pixel object temperature (deg C) into dev->To from the last
 * frame. emissivity ~0.95, tr = reflected temperature (e.g. Ta - 8). */
void  mlx90640_calculate_to     (mlx90640_t *dev, float emissivity, float tr);

/* Sub-page number (0/1) of the last frame read. */
int   mlx90640_get_subpage      (const mlx90640_t *dev);

/* Runtime configuration (control register 1). */
int   mlx90640_set_refresh_rate (mlx90640_t *dev, uint8_t rr);   /* MLX90640_RR_* */
int   mlx90640_set_resolution   (mlx90640_t *dev, uint8_t res);  /* MLX90640_RES_**/
int   mlx90640_set_chess_mode   (mlx90640_t *dev);               /* default mode  */

#ifdef __cplusplus
}
#endif

#endif /* MLX90640_H */
