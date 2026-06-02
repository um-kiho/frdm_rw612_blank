/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Sleep-light colour LUT for SK6812 RGBW strips.
 *
 * Direct port of the ESP32 reference (`main/led/sleep_light_table.h`),
 * kept bit-for-bit identical so the same preset IDs and reference RGBW
 * triplets keep their meaning across both platforms.
 *
 * --------------------------------------------------------------------------
 * [Design summary - sleep-friendly lighting]
 * --------------------------------------------------------------------------
 * | Item             | Recommendation                                     |
 * |------------------|----------------------------------------------------|
 * | Colour temp.     | 1800-2700 K (amber / warm). Suppress blue.         |
 * | Blue channel (B) | 0 or near-0 to avoid ipRGC / melatonin suppression.|
 * | White (W) LED    | Often high-CCT on RGBW strips; keep 0 or minimal.  |
 * | Brightness       | After dark adaptation, a few nit is plenty.        |
 * | Ramping          | Fade in/out rather than hard switch on.            |
 * | Wake-aid level   | "Visible" rather than "bright" warm light.         |
 * --------------------------------------------------------------------------
 *
 * Preset table (values are the RGBW peak at brightness_pct = 100). The
 * presets were tuned against a conservative "dark bedroom" reference; if
 * the strip looks too bright in the actual install, dim further with the
 * sleep_light_get_scaled2() strip_gain_pct argument.
 *
 * --------------------------------------------------------------------------
 * | preset                        | use case                |   R   G   B   W  |
 * |-------------------------------|-------------------------|------------------|
 * | SLEEP_LIGHT_OFF               | off                     |   0   0   0   0  |
 * | SLEEP_LIGHT_STARLIGHT         | near-dark night-light   |   4   1   0   0  |
 * | SLEEP_LIGHT_DEEP_RED          | minimum-stimulus red    |  10   0   0   0  |
 * | SLEEP_LIGHT_AMBER_2200K       | moonlight, default      |  22   6   0   0  |
 * | SLEEP_LIGHT_WARM_2700K        | slightly brighter warm  |  30  12   0   0  |
 * | SLEEP_LIGHT_PRE_SLEEP         | pre-bed task light      |  38  18   0   0  |
 * | SLEEP_LIGHT_NIGHT_AMBULATORY  | midnight walk / feeding |  45  22   0   4  |
 * | SLEEP_LIGHT_DAWN_WARMUP       | gentle dawn wake-up     |  35  20   2   6  |
 * --------------------------------------------------------------------------
 *
 * Usage on RW612:
 * @code
 *   sleep_light_rgbw_t c;
 *   sleep_light_get_scaled(SLEEP_LIGHT_AMBER_2200K, 40, &c);
 *   sk6812_fill(sleep_light_to_sk6812(c));
 *   sk6812_refresh();
 * @endcode
 *
 * Or shorter through led_task:
 * @code
 *   led_task_set_sleep_light(SLEEP_LIGHT_AMBER_2200K, 40);
 * @endcode
 */

#ifndef APP_SLEEP_LIGHT_TABLE_H
#define APP_SLEEP_LIGHT_TABLE_H

#include <stdint.h>

#include "sk6812.h"

#ifdef __cplusplus
extern "C" {
#endif

/* RGBW values for one preset (same layout as ESP32 reference). The fields
 * happen to be the same as sk6812_color_t but we keep a distinct typedef
 * so callers can mix-and-match without an implicit conversion. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t w;
} sleep_light_rgbw_t;

typedef enum {
    SLEEP_LIGHT_OFF = 0,
    SLEEP_LIGHT_STARLIGHT,
    SLEEP_LIGHT_DEEP_RED,
    SLEEP_LIGHT_AMBER_2200K,
    SLEEP_LIGHT_WARM_2700K,
    SLEEP_LIGHT_PRE_SLEEP,
    SLEEP_LIGHT_NIGHT_AMBULATORY,
    SLEEP_LIGHT_DAWN_WARMUP,
    SLEEP_LIGHT_PRESET_COUNT
} sleep_light_preset_t;

/* Peak RGBW for each preset (brightness_pct = 100). Order matches the
 * preset enum above. ESP32-side values, kept identical. */
static const sleep_light_rgbw_t sleep_light_preset_table[SLEEP_LIGHT_PRESET_COUNT] = {
    { 0,   0,  0,  0},
    { 4,   1,  0,  0},
    {10,   0,  0,  0},
    {22,   6,  0,  0},
    {30,  12,  0,  0},
    {38,  18,  0,  0},
    {45,  22,  0,  4},
    {35,  20,  2,  6},
};

/* Typical UI/dial brightness steps (0-100%). For a 7-position knob:
 *   step 0 = off, step 6 = full preset. */
static const uint8_t sleep_light_brightness_steps[] = {0, 5, 10, 20, 35, 55, 75, 100};
#define SLEEP_LIGHT_BRIGHTNESS_STEP_COUNT  \
    (sizeof(sleep_light_brightness_steps) / sizeof(sleep_light_brightness_steps[0]))

/* Resolve a preset to a brightness-scaled RGBW triplet.
 *   brightness_pct in [0..100]; values > 100 are clamped. */
static inline void sleep_light_get_scaled(sleep_light_preset_t preset,
                                          uint8_t brightness_pct,
                                          sleep_light_rgbw_t *out)
{
    if ((unsigned)preset >= (unsigned)SLEEP_LIGHT_PRESET_COUNT || out == 0) {
        if (out) {
            out->r = out->g = out->b = out->w = 0;
        }
        return;
    }
    if (brightness_pct > 100u) brightness_pct = 100u;
    const sleep_light_rgbw_t *b = &sleep_light_preset_table[preset];
    out->r = (uint8_t)((uint16_t)b->r * brightness_pct / 100u);
    out->g = (uint8_t)((uint16_t)b->g * brightness_pct / 100u);
    out->b = (uint8_t)((uint16_t)b->b * brightness_pct / 100u);
    out->w = (uint8_t)((uint16_t)b->w * brightness_pct / 100u);
}

/* Same as above but with an extra strip_gain_pct factor (also clamped to
 * 100). Useful when the installed strip is brighter than the reference
 * the preset values were tuned against. Final = preset * (b/100) * (g/100). */
static inline void sleep_light_get_scaled2(sleep_light_preset_t preset,
                                           uint8_t brightness_pct,
                                           uint8_t strip_gain_pct,
                                           sleep_light_rgbw_t *out)
{
    if (strip_gain_pct > 100u) strip_gain_pct = 100u;
    sleep_light_get_scaled(preset, brightness_pct, out);
    if (out == 0) return;
    out->r = (uint8_t)((uint16_t)out->r * strip_gain_pct / 100u);
    out->g = (uint8_t)((uint16_t)out->g * strip_gain_pct / 100u);
    out->b = (uint8_t)((uint16_t)out->b * strip_gain_pct / 100u);
    out->w = (uint8_t)((uint16_t)out->w * strip_gain_pct / 100u);
}

/* Bridge to RW612's sk6812 driver type. */
static inline sk6812_color_t sleep_light_to_sk6812(sleep_light_rgbw_t in)
{
    return SK6812_RGBW(in.r, in.g, in.b, in.w);
}

/* Convenience: build an sk6812_color_t straight from a preset + percent. */
static inline sk6812_color_t sleep_light_color(sleep_light_preset_t preset,
                                               uint8_t brightness_pct)
{
    sleep_light_rgbw_t c;
    sleep_light_get_scaled(preset, brightness_pct, &c);
    return sleep_light_to_sk6812(c);
}

#ifdef __cplusplus
}
#endif

#endif /* APP_SLEEP_LIGHT_TABLE_H */
