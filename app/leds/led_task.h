/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Animation task for the SK6812RGBW strip.
 *
 * A single FreeRTOS Timer drives a 50 Hz (20 ms) refresh of the LED strip
 * based on a pattern + base-colour state. The pattern engine produces the
 * per-pixel colours each tick and then calls sk6812_refresh().
 *
 *   LED_PAT_OFF        : strip cleared, no SPI traffic after the clear.
 *   LED_PAT_SOLID      : every pixel = base, refresh only when dirty.
 *   LED_PAT_BREATHING  : whole strip = base scaled by triangular envelope.
 *                        4 s period (0 -> peak -> 0), peak = current
 *                        brightness setting.
 *   LED_PAT_RAINBOW    : HSV hue rotates across pixels and time. base.w
 *                        is forwarded as a white-floor for legibility.
 *   LED_PAT_STROBE     : base / off square wave, 100 ms on, 100 ms off.
 *
 * Producers (BLE service, state machine, debug shell) talk to this module
 * via the setters below. The setters are non-blocking and one-shot;
 * the task picks them up on the next tick.
 */

#ifndef APP_LED_TASK_H
#define APP_LED_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "sk6812.h"
#include "sleep_light_table.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum led_pattern {
    LED_PAT_OFF       = 0,
    LED_PAT_SOLID     = 1,
    LED_PAT_BREATHING = 2,
    LED_PAT_RAINBOW   = 3,
    LED_PAT_STROBE    = 4,
} led_pattern_t;

#ifndef APP_LED_TASK_TICK_MS
#define APP_LED_TASK_TICK_MS    20u
#endif

/* Brings up sk6812 (if not already), creates the timer + Mutex, leaves the
 * strip in LED_PAT_OFF. Safe to call multiple times. */
int  led_task_start(void);

/* Atomically replace the active pattern and its base colour. The pattern
 * engine reads both together on the next tick. */
void led_task_set_pattern(led_pattern_t p, sk6812_color_t base);

/* Just change the base colour (keeps the current pattern). */
void led_task_set_color(sk6812_color_t base);

/* 0..255 q8 scaler. Stored in sk6812.c. */
void led_task_set_brightness(uint8_t br);

/* Direct per-pixel override. Only honoured when current pattern is SOLID
 * or OFF; otherwise the animation overwrites it on the next tick.
 * Returns 0 on success. */
int  led_task_set_pixel(uint16_t idx, sk6812_color_t c);

/* Resolve a sleep-light preset (sleep_light_table.h) at the requested
 * percent brightness and switch the strip to LED_PAT_SOLID with that
 * colour. brightness_pct is clamped to [0..100].
 *
 * The preset / brightness pair is also cached so a subsequent
 * led_task_get_sleep_light() returns the values last applied. This is
 * what svc_led exposes to the phone for the bedtime UI. */
void led_task_set_sleep_light(sleep_light_preset_t preset,
                              uint8_t brightness_pct);

/* Last applied sleep-light values. Both pointers may be NULL. If the
 * strip is currently driven by something other than set_sleep_light()
 * (a pattern animation, a direct set_color(), ...), the returned values
 * are the *most recent* sleep-light selection, not the live state. */
void led_task_get_sleep_light(sleep_light_preset_t *preset_out,
                              uint8_t *brightness_pct_out);

/* Readbacks for the BLE status characteristic. */
led_pattern_t  led_task_get_pattern(void);
sk6812_color_t led_task_get_color(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_LED_TASK_H */
