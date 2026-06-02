/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SK6812RGBW driver core - pixel buffer + GRBW encoder.
 *
 * Threading model:
 *   sk6812_refresh() takes a Zephyr mutex so multiple producer tasks
 *   (state machine, BLE control, debug shell, ...) can call it without
 *   tearing a SPI transaction. Setters do NOT take the mutex; they're
 *   one-word stores and the encoder reads them under the same mutex.
 */

#include "sk6812.h"
#include "sk6812_io_nxp_spi.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sk6812, LOG_LEVEL_INF);

/* ----------------------------- state ----------------------------------- */
static sk6812_color_t   s_pixels[APP_LED_COUNT];
static uint8_t          s_brightness = 0xFFu;
static uint8_t          s_tx[APP_LED_TX_BUFFER_BYTES];
static struct k_mutex   s_mtx;
static bool             s_initialised;

/* --------------------------- helpers ----------------------------------- */
static inline uint8_t scale8(uint8_t v, uint8_t s)
{
    if (s == 0xFFu) return v;
    return (uint8_t)(((uint16_t)v * (uint16_t)s + 128u) / 255u);
}

/* Expand one channel byte (MSB-first) into 8 SPI bytes at dst. */
static inline void encode_byte(uint8_t v, uint8_t *dst)
{
    for (int i = 7; i >= 0; --i) {
        dst[7 - i] = ((v >> i) & 0x1u) ? APP_LED_SPI_BIT_ONE
                                       : APP_LED_SPI_BIT_ZERO;
    }
}

/* --------------------------- public API -------------------------------- */
int sk6812_init(void)
{
    if (s_initialised) {
        /* second call: just reset pixels + push. */
        memset(s_pixels, 0, sizeof(s_pixels));
        return sk6812_refresh();
    }

    k_mutex_init(&s_mtx);

    if (sk6812_io_nxp_spi_init() != 0) {
        LOG_ERR("spi init failed");
        return -2;
    }

    memset(s_pixels, 0, sizeof(s_pixels));
    s_initialised = true;

    /* Send one full frame of zeros so the strip starts in a known state
     * even if the previous boot left garbage in the line. */
    return sk6812_refresh();
}

uint16_t sk6812_count(void)
{
    return (uint16_t)APP_LED_COUNT;
}

int sk6812_set_pixel(uint16_t idx, sk6812_color_t c)
{
    if (idx >= APP_LED_COUNT) return -1;
    s_pixels[idx] = c;
    return 0;
}

int sk6812_fill(sk6812_color_t c)
{
    for (uint16_t i = 0; i < APP_LED_COUNT; ++i) s_pixels[i] = c;
    return 0;
}

/* Percent-based scale helpers (ESP32 reference compatible). The result is
 * stored back into the pixel buffer so subsequent refreshes keep the
 * dimmed colour - mirrors sk6812rgbw_fill_scaled() / set_pixel_scaled(). */
static inline sk6812_color_t scale_pct(sk6812_color_t c, uint8_t pct)
{
    if (pct > 100u) pct = 100u;
    sk6812_color_t out;
    out.r = (uint8_t)((uint16_t)c.r * pct / 100u);
    out.g = (uint8_t)((uint16_t)c.g * pct / 100u);
    out.b = (uint8_t)((uint16_t)c.b * pct / 100u);
    out.w = (uint8_t)((uint16_t)c.w * pct / 100u);
    return out;
}

int sk6812_fill_scaled(sk6812_color_t c, uint8_t brightness_pct)
{
    return sk6812_fill(scale_pct(c, brightness_pct));
}

int sk6812_set_pixel_scaled(uint16_t idx, sk6812_color_t c, uint8_t brightness_pct)
{
    return sk6812_set_pixel(idx, scale_pct(c, brightness_pct));
}

int sk6812_clear(void)
{
    (void)sk6812_fill(SK6812_OFF);
    return sk6812_refresh();
}

void sk6812_set_brightness(uint8_t scale)
{
    s_brightness = scale;
}

uint8_t sk6812_get_brightness(void)
{
    return s_brightness;
}

int sk6812_refresh(void)
{
    if (!s_initialised) return -1;
    if (k_mutex_lock(&s_mtx, K_FOREVER) != 0) return -2;

    uint8_t  br = s_brightness;
    uint8_t *p  = s_tx;

    for (uint16_t i = 0; i < APP_LED_COUNT; ++i) {
        sk6812_color_t c = s_pixels[i];
        uint8_t g = scale8(c.g, br);
        uint8_t r = scale8(c.r, br);
        uint8_t b = scale8(c.b, br);
        uint8_t w = scale8(c.w, br);

        /* SK6812RGBW byte order is G R B W, MSB-first per byte. */
        encode_byte(g, p); p += 8;
        encode_byte(r, p); p += 8;
        encode_byte(b, p); p += 8;
        encode_byte(w, p); p += 8;
    }
    /* Reset pulse: hold line low. */
    memset(p, 0x00, APP_LED_RESET_BYTES);

    int rc = sk6812_io_nxp_spi_send_blocking(s_tx, sizeof(s_tx));

    k_mutex_unlock(&s_mtx);
    return rc;
}
