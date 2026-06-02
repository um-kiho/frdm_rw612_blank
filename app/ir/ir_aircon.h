/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * High-level air conditioner control on top of ir_tx + ir_codec_*.
 *
 * RW612 port of the ESP32 `ir_aircon_control` module - the brand /
 * action surface is identical so existing phone-side enums keep working.
 *
 *   POWER ON     -> Samsung: ir_samsung_ac_send(on_21)
 *                            cache 14-byte body for subsequent temp ops
 *   POWER OFF    -> Samsung: ir_samsung_ac_send(off_21)
 *   TEMP UP/DOWN -> Samsung: clamp + samsung_set_temp + fix checksums
 *                            + send the cached 14-byte state
 *   TEMP_SET     -> same as UP/DOWN but to an absolute temperature
 *
 * The exact frame bytes (k_samsung_ac_*) are copied unchanged from the
 * tested ESP32 build, so the receiver behaviour stays bit-for-bit
 * identical - only the timer / GPIO backend is new.
 *
 * Other brands (LG, Carrier, Winia, Paseco, Hisense) keep the same API
 * shape, but the codec backends are not yet ported. Calling them returns
 * IR_AIRCON_ERR_NOT_IMPL so the upper layers can hook them in later.
 */

#ifndef APP_IR_AIRCON_H
#define APP_IR_AIRCON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IR_AIRCON_BRAND_SAMSUNG = 0,
    IR_AIRCON_BRAND_LG,
    IR_AIRCON_BRAND_CARRIER_KR,
    IR_AIRCON_BRAND_WINIA,
    IR_AIRCON_BRAND_PASECO,
    IR_AIRCON_BRAND_HISENSE,
    IR_AIRCON_BRAND_COUNT
} ir_aircon_brand_t;

typedef enum {
    IR_AIRCON_ACTION_POWER_ON = 0,
    IR_AIRCON_ACTION_POWER_OFF,
    IR_AIRCON_ACTION_TEMP_UP,
    IR_AIRCON_ACTION_TEMP_DOWN,
} ir_aircon_action_t;

/* Samsung AC ranges (IRremoteESP8266). */
#define IR_AIRCON_SAMSUNG_TEMP_MIN_C    16u
#define IR_AIRCON_SAMSUNG_TEMP_MAX_C    30u

/* Error codes (>=0 means symbol count sent). */
#define IR_AIRCON_OK                     0
#define IR_AIRCON_ERR_NOT_READY         -1
#define IR_AIRCON_ERR_INVALID_ARG       -2
#define IR_AIRCON_ERR_INVALID_STATE     -3
#define IR_AIRCON_ERR_NOT_IMPL          -4
#define IR_AIRCON_ERR_TX                -5
#define IR_AIRCON_ERR_ENCODE            -6

/* One-shot init; safe to call multiple times. Resets cached state. */
int  ir_aircon_init(void);

/* Brand/action dispatcher. Blocks until the IR frame leaves the wire. */
int  ir_aircon_send(ir_aircon_brand_t brand, ir_aircon_action_t action);

/* Absolute temperature in °C. Outside [MIN..MAX] it is clamped.
 * For Samsung it requires that POWER_ON has been sent already (the
 * 14-byte body needs to be initialised first); otherwise returns
 * IR_AIRCON_ERR_INVALID_STATE - identical to the ESP32 behaviour. */
int  ir_aircon_set_temp_c(ir_aircon_brand_t brand, uint8_t temp_c);

/* Current cached temperature. Returns 0 if power is off / unknown. */
uint8_t ir_aircon_get_temp_c(ir_aircon_brand_t brand);

/* Power state of the cached brand model (Samsung only for now). */
bool ir_aircon_is_powered(ir_aircon_brand_t brand);

/* Lower-level escape hatch: send an arbitrary Samsung AC frame already
 * laid out in the 7-byte section LSB-first format (14 or 21 bytes).
 * The encoder fills in section checksums when fix_cs is true. */
int  ir_aircon_samsung_send_raw(const uint8_t *frame, size_t len, bool fix_cs);

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_AIRCON_H */
