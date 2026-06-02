/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Samsung air-conditioner (SAMSUNG_AC) IR encoder.
 *
 * Direct port of the ESP32 ir_samsung_ac_encoder.c (zCube revision /
 * IRremoteESP8266 ir_Samsung.cpp). The pulse durations and the LSB-first
 * 7-byte-per-section layout are unchanged; only the output stage was
 * adapted - instead of building rmt_symbol_word_t entries for ESP-IDF's
 * RMT engine we build ir_symbol_t entries that feed ir_tx.h.
 *
 * Frame layout, 38 kHz carrier:
 *
 *   [HDR     mark 690 us | space 17844 us ]
 *   [SEC_HDR mark 3086us | space 8864 us  ]
 *   [56 data bits, LSB-first: bit_mark 586 us + space (1=1432, 0=436)]
 *   [FOOT    mark 586 us | space 2886 us ]                  -- section gap
 *   ... repeat SEC_HDR..FOOT for each 7-byte section (N = 2 or 3) ...
 *   [FINAL   mark 0     | space 20000 us]                   -- latch gap
 *
 * Symbol count = 1 (header) + N * (1 + 56 + 1) = 1 + 58*N. So:
 *   N=2 (14-byte state): 117 symbols
 *   N=3 (21-byte ON/OFF): 175 symbols
 *
 * Both fit comfortably under APP_IR_TX_MAX_SYMBOLS = 384.
 */

#ifndef APP_IR_CODEC_SAMSUNG_AC_H
#define APP_IR_CODEC_SAMSUNG_AC_H

#include <stddef.h>
#include <stdint.h>

#include "ir_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IRremoteESP8266 ir_Samsung.cpp timings - shared with ESP32 encoder. */
#define IR_SAC_HDR_MARK_US        690u
#define IR_SAC_HDR_SPACE_US     17844u
#define IR_SAC_SEC_MARK_US       3086u
#define IR_SAC_SEC_SPACE_US      8864u
#define IR_SAC_GAP_US            2886u
#define IR_SAC_BIT_MARK_US        586u
#define IR_SAC_ONE_SPACE_US      1432u
#define IR_SAC_ZERO_SPACE_US      436u
#define IR_SAC_FINAL_GAP_US     20000u

/* Frame size limits. */
#define IR_SAC_SECTION_BYTES        7u
#define IR_SAC_STATE_BYTES         14u   /* 7 x 2 (typical state)   */
#define IR_SAC_EXT_STATE_BYTES     21u   /* 7 x 3 (ON / OFF frame)  */
#define IR_SAC_MAX_BYTES           IR_SAC_EXT_STATE_BYTES

/* Max ir_symbol_t entries the encoder can produce for one transmit. */
#define IR_SAC_MAX_SYMBOLS    (1u + (1u + 56u + 1u) * (IR_SAC_MAX_BYTES / IR_SAC_SECTION_BYTES))

/* Encode `data` (must be a multiple of 7, max IR_SAC_MAX_BYTES) into the
 * caller-supplied `out` array. Returns the number of symbols written, or
 * <0 on error.
 *
 *   out_cap must be >= 1 + 58 * (len / 7)
 */
int ir_codec_samsung_ac_encode(const uint8_t *data, size_t len,
                               ir_symbol_t *out, size_t out_cap);

/* Compute & write IRremoteESP8266-style section checksum into section[].
 * Useful for callers that mutate fields (e.g. temperature) in-place. */
void ir_codec_samsung_ac_fix_checksum(uint8_t *section);

/* Convenience: fix both sections of a 14-byte state buffer. */
void ir_codec_samsung_ac_fix_checksums_14(uint8_t *state14);

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_CODEC_SAMSUNG_AC_H */
