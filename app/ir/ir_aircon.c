/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * High-level air conditioner state + IR dispatcher.
 *
 * RW612 port of ir_aircon_control.c. Frame bytes (k_samsung_ac_on_21,
 * k_samsung_ac_off_21, k_samsung_ac_on_14) and temperature byte layout
 * are kept identical to the tested ESP32 build to preserve compatibility
 * with the existing air-conditioner units.
 */

#include "ir_aircon.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ir_tx.h"
#include "ir_codec_samsung_ac.h"

LOG_MODULE_REGISTER(app_ir_aircon, LOG_LEVEL_INF);

/* 실측 리모컨 코드 (2026-06-05). 섹션2가 내장 기본값과 다름 — 사용자 에어컨 전용.
 * (섹션0/1 은 기존과 동일.) */
static const uint8_t k_samsung_ac_on_21[] = {
    0x02, 0x92, 0x0F, 0x00, 0x00, 0x00, 0xF0,
    0x01, 0xD2, 0x0F, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x52, 0x0F, 0x00, 0x20, 0x11, 0xF8,
};
static const uint8_t k_samsung_ac_off_21[] = {
    0x02, 0xB2, 0x0F, 0x00, 0x00, 0x00, 0xC0,
    0x01, 0xD2, 0x0F, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x72, 0x0F, 0x00, 0x20, 0x11, 0xC8,
};
/* Steady-state 14-byte frame derived from sections 1 and 3 of on_21 -
 * this is what IRSamsungAc::send() emits after the initial power-on. */
static const uint8_t k_samsung_ac_on_14[] = {
    0x02, 0x92, 0x0F, 0x00, 0x00, 0x00, 0xF0,
    0x01, 0xE2, 0xFE, 0x71, 0x80, 0x11, 0xF0,
};
#if 0
/* Learned-button fallback path used temporarily by rotary switch sequence.
 * Names are kept stable to match field-debug notes. */
static const uint8_t s_samsung_ac_learned_on_7[] = {
    0x02, 0x92, 0x0F, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t s_samsung_ac_learned_temp_up_7[] = {
    0x01, 0xE2, 0xFE, 0x71, 0x90, 0x11, 0x00,
};
static const uint8_t s_samsung_ac_learned_temp_down_7[] = {
    0x01, 0xE2, 0xFE, 0x71, 0x70, 0x11, 0x00,
};
static const uint8_t s_samsung_ac_learned_off_14[] = {
    0x02, 0xB2, 0x0F, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x72, 0x0F, 0x00, 0x20, 0x11, 0x00,
};
#endif
/* 캡처 기반 Samsung AC 패턴 (사용자 로그 2026-06-08) */
static const uint8_t s_samsung_ac_learned_off_14[] = {
    0x01, 0xC2, 0x0F, 0x00, 0x00, 0x20, 0x00,
    0x01, 0x02, 0xFF, 0x01, 0x80, 0x1F, 0xC0,
};
static const uint8_t s_samsung_ac_learned_on_7[] = {
    0x01, 0xE2, 0xFE, 0x01, 0x60, 0x1B, 0xF0,
};
static const uint8_t s_samsung_ac_learned_temp_up_7[] = {
    0x01, 0xD2, 0xFE, 0x01, 0x70, 0x1B, 0xF0,
};
static const uint8_t s_samsung_ac_learned_temp_down_7[] = {
    0x01, 0xE2, 0xFE, 0x01, 0x50, 0x1B, 0xF0,
};
/* Temperature is the high nibble of byte index 11 (5th byte of section 2,
 * = (°C - 16) -> 16°C → 0x0 ... 30°C → 0xE).
 * After the nibble is touched, both section checksums must be rewritten. */
#define IR_SAC_TEMP_BYTE_INDEX  11u

/* ---------------------------------------------------------------------- *
 * Module state - cached Samsung state, mutex protects in-place edits.
 * ---------------------------------------------------------------------- */
static uint8_t           s_sam14[14];
static bool              s_sam_power;
static struct k_mutex    s_mtx;
static bool              s_inited;

static int send_symbol_frame(const uint8_t *bytes, size_t len)
{
    static ir_symbol_t syms[IR_SAC_MAX_SYMBOLS];
    int n = ir_codec_samsung_ac_encode(bytes, len, syms, IR_SAC_MAX_SYMBOLS);
    if (n < 0) return IR_AIRCON_ERR_ENCODE;
    int rc = ir_tx_send_symbols(syms, (size_t)n);
    return (rc == 0) ? IR_AIRCON_OK : IR_AIRCON_ERR_TX;
}

/* ---------------------------------------------------------------------- *
 * Samsung helpers
 * ---------------------------------------------------------------------- */
static uint8_t samsung_get_temp_c_locked(void)
{
    return (uint8_t)(((s_sam14[IR_SAC_TEMP_BYTE_INDEX] >> 4) & 0x0Fu)
                     + IR_AIRCON_SAMSUNG_TEMP_MIN_C);
}

static void samsung_set_temp_c_locked(uint8_t temp_c)
{
    if (temp_c < IR_AIRCON_SAMSUNG_TEMP_MIN_C) temp_c = IR_AIRCON_SAMSUNG_TEMP_MIN_C;
    if (temp_c > IR_AIRCON_SAMSUNG_TEMP_MAX_C) temp_c = IR_AIRCON_SAMSUNG_TEMP_MAX_C;
    uint8_t nib = (uint8_t)(temp_c - IR_AIRCON_SAMSUNG_TEMP_MIN_C);
    uint8_t *b = &s_sam14[IR_SAC_TEMP_BYTE_INDEX];
    *b = (uint8_t)((*b & 0x0Fu) | ((nib & 0x0Fu) << 4));
}

static int samsung_send_state_with_temp_locked(uint8_t temp_c)
{
    if (!s_sam_power) return IR_AIRCON_ERR_INVALID_STATE;
    samsung_set_temp_c_locked(temp_c);
    ir_codec_samsung_ac_fix_checksums_14(s_sam14);
    return send_symbol_frame(s_sam14, sizeof(s_sam14));
}

static int send_learned_raw(const uint8_t *frame, size_t len)
{
    if (frame == NULL) return IR_AIRCON_ERR_INVALID_ARG;
    if (len == 0u || (len % 7u) != 0u || len > IR_SAC_MAX_BYTES)
        return IR_AIRCON_ERR_INVALID_ARG;

    return send_symbol_frame(frame, len);
}

static int exec_samsung_learned(ir_aircon_action_t action)
{
    int rc;
    switch (action) {
    case IR_AIRCON_ACTION_POWER_ON:
        rc = send_learned_raw(s_samsung_ac_learned_on_7,
                              sizeof(s_samsung_ac_learned_on_7));
        if (rc == IR_AIRCON_OK) {
            memcpy(s_sam14, k_samsung_ac_on_14, sizeof(s_sam14));
            s_sam_power = true;
        }
        return rc;

    case IR_AIRCON_ACTION_POWER_OFF:
        rc = send_learned_raw(s_samsung_ac_learned_off_14,
                              sizeof(s_samsung_ac_learned_off_14));
        if (rc == IR_AIRCON_OK) s_sam_power = false;
        return rc;

    case IR_AIRCON_ACTION_TEMP_UP:
        return send_learned_raw(s_samsung_ac_learned_temp_up_7,
                                sizeof(s_samsung_ac_learned_temp_up_7));

    case IR_AIRCON_ACTION_TEMP_DOWN:
        return send_learned_raw(s_samsung_ac_learned_temp_down_7,
                                sizeof(s_samsung_ac_learned_temp_down_7));

    default:
        return IR_AIRCON_ERR_INVALID_ARG;
    }
}

/* ---------------------------------------------------------------------- *
 * Public API
 * ---------------------------------------------------------------------- */
int ir_aircon_init(void)
{
    if (!s_inited) {
        k_mutex_init(&s_mtx);
        s_inited = true;
    }

    k_mutex_lock(&s_mtx, K_FOREVER);
    memset(s_sam14, 0, sizeof(s_sam14));
    s_sam_power = false;
    k_mutex_unlock(&s_mtx);

    if (!ir_tx_is_ready()) {
        int rc = ir_tx_init();
        if (rc != 0) return IR_AIRCON_ERR_NOT_READY;
    }
    LOG_INF("ready");
    return IR_AIRCON_OK;
}

int ir_aircon_send(ir_aircon_brand_t brand, ir_aircon_action_t action)
{
    if (!s_inited || !ir_tx_is_ready()) return IR_AIRCON_ERR_NOT_READY;
    if ((unsigned)action > IR_AIRCON_ACTION_TEMP_DOWN) return IR_AIRCON_ERR_INVALID_ARG;

    k_mutex_lock(&s_mtx, K_FOREVER);
    int rc;
    switch (brand) {
    case IR_AIRCON_BRAND_SAMSUNG:
        rc = exec_samsung_learned(action);
        break;
    case IR_AIRCON_BRAND_LG:
    case IR_AIRCON_BRAND_CARRIER_KR:
    case IR_AIRCON_BRAND_WINIA:
    case IR_AIRCON_BRAND_PASECO:
    case IR_AIRCON_BRAND_HISENSE:
        rc = IR_AIRCON_ERR_NOT_IMPL;
        break;
    default:
        rc = IR_AIRCON_ERR_INVALID_ARG;
        break;
    }
    k_mutex_unlock(&s_mtx);
    return rc;
}

int ir_aircon_set_temp_c(ir_aircon_brand_t brand, uint8_t temp_c)
{
    if (!s_inited || !ir_tx_is_ready())    return IR_AIRCON_ERR_NOT_READY;
    if (brand != IR_AIRCON_BRAND_SAMSUNG)   return IR_AIRCON_ERR_NOT_IMPL;

    k_mutex_lock(&s_mtx, K_FOREVER);
    int rc = samsung_send_state_with_temp_locked(temp_c);
    k_mutex_unlock(&s_mtx);
    return rc;
}

uint8_t ir_aircon_get_temp_c(ir_aircon_brand_t brand)
{
    if (brand != IR_AIRCON_BRAND_SAMSUNG) return 0;
    if (!s_inited) return 0;
    k_mutex_lock(&s_mtx, K_FOREVER);
    uint8_t t = s_sam_power ? samsung_get_temp_c_locked() : 0;
    k_mutex_unlock(&s_mtx);
    return t;
}

bool ir_aircon_is_powered(ir_aircon_brand_t brand)
{
    if (brand != IR_AIRCON_BRAND_SAMSUNG) return false;
    return s_sam_power;
}

int ir_aircon_samsung_send_raw(const uint8_t *frame, size_t len, bool fix_cs)
{
    if (!ir_tx_is_ready()) return IR_AIRCON_ERR_NOT_READY;
    if (frame == NULL)     return IR_AIRCON_ERR_INVALID_ARG;
    if (len == 0u || (len % 7u) != 0u || len > IR_SAC_MAX_BYTES)
        return IR_AIRCON_ERR_INVALID_ARG;

    /* Make an internal copy so the caller's buffer can be discarded
     * (and so we may mutate the checksum nibbles if requested). */
    static uint8_t scratch[IR_SAC_MAX_BYTES];
    memcpy(scratch, frame, len);

    if (fix_cs) {
        for (size_t s = 0; s < len; s += 7u) {
            ir_codec_samsung_ac_fix_checksum(&scratch[s]);
        }
    }
    return send_symbol_frame(scratch, len);
}
