/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Transport-agnostic IR transmit API (RW612 port of the ESP32 RMT-based
 * `ir_tv` module).
 *
 * Model: callers build an array of "symbols" - each symbol is a pair of
 * (mark, space) microsecond durations, exactly mirroring the ESP32 RMT
 * `rmt_symbol_word_t {level0=1,duration0,level1=0,duration1}` layout. The
 * driver clocks the mark/space pairs out on a GPIO with a 38 kHz (or
 * caller-selected) carrier active during the *mark* half only.
 *
 *      mark = TX LED ON  (carrier modulated)
 *      space = TX LED OFF (no carrier)
 *
 * This is the same convention used by IRremoteESP8266 and by the original
 * ESP32 encoders in `c:\work\new_zcube\PGM\New_zCube _cursor\main\ir_tv`.
 * Encoder modules (`ir_codec_samsung_ac.c`, ...) only deal with symbols
 * and bytes; they do not know which timer drives the wire.
 *
 * The backend currently provided is `ir_tx_nxp_ctimer.c` which uses CTimer
 * match interrupts to walk the symbol array and an SCTimer PWM channel to
 * produce the 38 kHz carrier (gated on the GPIO output mux).
 */

#ifndef APP_IR_TX_H
#define APP_IR_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors `rmt_symbol_word_t` in ESP-IDF: each entry holds one mark and
 * one trailing space. The ESP32 encoder modules build arrays of these,
 * so keeping the field names/units identical lets us copy the encoder
 * logic verbatim. */
typedef struct ir_symbol {
    uint16_t mark_us;       /* level=1 (carrier on) duration  */
    uint16_t space_us;      /* level=0 (carrier off) duration */
} ir_symbol_t;

#ifndef APP_IR_TX_DEFAULT_CARRIER_HZ
#define APP_IR_TX_DEFAULT_CARRIER_HZ   38000u
#endif

#ifndef APP_IR_TX_MAX_SYMBOLS
/* Samsung AC frame uses ~117 symbols for 14 byte body, ~175 for 21 byte.
 * Keep a comfortable margin so future encoders (LG, NEC, RC5/6, ...) fit. */
#define APP_IR_TX_MAX_SYMBOLS          384u
#endif

#ifndef APP_IR_TX_SEND_TIMEOUT_MS
#define APP_IR_TX_SEND_TIMEOUT_MS      3000u
#endif

/* One-shot init: configures the GPIO, the carrier generator and the
 * mark/space sequencer. Safe to call more than once. */
int  ir_tx_init(void);
void ir_tx_deinit(void);
bool ir_tx_is_ready(void);

/* Change the carrier frequency in the carrier generator (e.g. 36 kHz for
 * RC-5/6, 40 kHz for Sony SIRC, 38 kHz for NEC/Samsung). Returns 0 on
 * success. The backend may snap to the nearest representable value. */
int  ir_tx_set_carrier_hz(uint32_t hz);
uint32_t ir_tx_get_carrier_hz(void);

/* Blocking send. The caller owns `syms` for the duration of the call; the
 * backend may either DMA from it directly or maintain its own internal
 * copy. Returns 0 on success. */
int  ir_tx_send_symbols(const ir_symbol_t *syms, size_t count);

/* Optional small helper used by every encoder. */
static inline ir_symbol_t ir_symbol_make(uint16_t mark_us, uint16_t space_us)
{
    ir_symbol_t s = { mark_us, space_us };
    return s;
}

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_TX_H */
