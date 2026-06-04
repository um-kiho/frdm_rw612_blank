/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * SAMSUNG_AC encoder - port of ESP32 ir_samsung_ac_encoder.c / ESP8266
 * IRSamsungAc to the ir_symbol_t array convention used by ir_tx.h.
 */

#include "ir_codec_samsung_ac.h"

#include <string.h>

/* ---------------------------------------------------------------------- *
 * IRSamsungAc::calcSectionChecksum
 *
 * Sum of popcounts:
 *   - section[0]
 *   - low nibble of section[1]
 *   - high nibble of section[2]
 *   - section[3..6]
 * XOR with 0xFF; low nibble goes to section[1] high nibble; high nibble
 * goes to section[2] low nibble (sic - see IRremoteESP8266 source).
 * ---------------------------------------------------------------------- */
static uint8_t popcount_u8(uint8_t v)
{
    uint8_t n = 0;
    while (v) { n = (uint8_t)(n + (v & 1u)); v >>= 1; }
    return n;
}

static uint8_t popcount_buf(const uint8_t *p, size_t len)
{
    uint16_t t = 0;
    for (size_t i = 0; i < len; ++i) t = (uint16_t)(t + popcount_u8(p[i]));
    return (uint8_t)t;
}

static uint8_t sec_checksum(const uint8_t *s)
{
    uint16_t sum = 0;
    sum  = (uint16_t)(sum + popcount_u8(s[0]));
    sum  = (uint16_t)(sum + popcount_u8((uint8_t)(s[1] & 0x0Fu)));
    sum  = (uint16_t)(sum + popcount_u8((uint8_t)((s[2] >> 4) & 0x0Fu)));
    sum  = (uint16_t)(sum + popcount_buf(s + 3, 4));
    return (uint8_t)((uint8_t)sum ^ 0xFFu);
}

void ir_codec_samsung_ac_fix_checksum(uint8_t *section)
{
    uint8_t cs = sec_checksum(section);
    section[1] = (uint8_t)((section[1] & 0x0Fu) | ((cs & 0x0Fu) << 4));
    section[2] = (uint8_t)((section[2] & 0xF0u) | ((cs >> 4) & 0x0Fu));
}

void ir_codec_samsung_ac_fix_checksums_14(uint8_t *state14)
{
    ir_codec_samsung_ac_fix_checksum(state14);
    ir_codec_samsung_ac_fix_checksum(state14 + 7);
}

bool ir_codec_samsung_ac_verify(const uint8_t *section)
{
    uint8_t cs     = sec_checksum(section);
    uint8_t stored = (uint8_t)(((section[2] & 0x0Fu) << 4) |
                               ((section[1] >> 4) & 0x0Fu));
    return cs == stored;
}

bool ir_codec_samsung_ac_verify_all(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0u || (len % IR_SAC_SECTION_BYTES) != 0u) {
        return false;
    }
    for (size_t s = 0; s < len; s += IR_SAC_SECTION_BYTES) {
        if (!ir_codec_samsung_ac_verify(&data[s])) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------- *
 * Symbol-array builder
 * ---------------------------------------------------------------------- */
static inline uint8_t bit_lsbf(const uint8_t *sec, unsigned k)
{
    return (uint8_t)((sec[k / 8u] >> (k % 8u)) & 1u);
}

int ir_codec_samsung_ac_encode(const uint8_t *data, size_t len,
                               ir_symbol_t *out, size_t out_cap)
{
    if (data == NULL || out == NULL)              return -1;
    if (len == 0u || (len % IR_SAC_SECTION_BYTES) != 0u) return -2;
    if (len > IR_SAC_MAX_BYTES)                   return -3;

    size_t nsec   = len / IR_SAC_SECTION_BYTES;
    size_t need   = 1u + (1u + 56u + 1u) * nsec + 0u;  /* hdr + sections; final_gap merged into last foot */
    if (out_cap < need)                           return -4;

    size_t n = 0;

    /* 1) Header. */
    out[n++] = ir_symbol_make(IR_SAC_HDR_MARK_US, IR_SAC_HDR_SPACE_US);

    for (size_t s = 0; s < nsec; ++s) {
        const uint8_t *sec = data + s * IR_SAC_SECTION_BYTES;

        /* 2) Section header. */
        out[n++] = ir_symbol_make(IR_SAC_SEC_MARK_US, IR_SAC_SEC_SPACE_US);

        /* 3) 56 data bits, LSB-first. */
        for (unsigned k = 0; k < 56u; ++k) {
            uint16_t sp = bit_lsbf(sec, k) ? IR_SAC_ONE_SPACE_US
                                           : IR_SAC_ZERO_SPACE_US;
            out[n++] = ir_symbol_make(IR_SAC_BIT_MARK_US, sp);
        }

        /* 4) Inter-section foot (or final gap if this is the last). */
        uint16_t sp = (s + 1u < nsec) ? IR_SAC_GAP_US
                                      : IR_SAC_FINAL_GAP_US;
        out[n++] = ir_symbol_make(IR_SAC_BIT_MARK_US, sp);
    }

    return (int)n;
}
