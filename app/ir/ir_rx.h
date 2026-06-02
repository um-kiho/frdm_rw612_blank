/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Transport-agnostic IR receive API (RW612 + TSDP341 receiver).
 *
 * The TSDP341 (38 kHz demodulating receiver) outputs an active-low signal:
 *   GPIO LOW  = IR carrier present = mark
 *   GPIO HIGH = no carrier / silence = space
 *
 * The backend measures the time between rising and falling edges and builds
 * ir_symbol_t arrays in the same mark_us / space_us format used by ir_tx.h.
 * This lets callers pass a received frame directly to ir_tx_send_symbols()
 * for retransmission, or into ir_decode() to identify the remote protocol.
 *
 * Callbacks fire from the Zephyr system workqueue (NOT from ISR context),
 * so the handler may safely call blocking functions like ir_tx_send_symbols().
 *
 * Frame boundary:
 *   The end of a frame is detected by silence longer than
 *   APP_IR_RX_GAP_THRESHOLD_US (default 10 ms).  The last ir_symbol_t in
 *   every frame always has space_us == 0 because the gap that follows it
 *   is silence, not a measured space.  Both ir_decode() and the retransmit
 *   path tolerate this trailing zero.
 */

#ifndef APP_IR_RX_H
#define APP_IR_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "ir_tx.h"   /* for ir_symbol_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum mark/space symbols buffered per frame.
 * NEC 32-bit = 67 syms; Samsung AC 21-byte = 175 syms; 384 covers all. */
#ifndef APP_IR_RX_MAX_SYMBOLS
#define APP_IR_RX_MAX_SYMBOLS         384u
#endif

/* Silence duration that marks end-of-frame (microseconds). */
#ifndef APP_IR_RX_GAP_THRESHOLD_US
#define APP_IR_RX_GAP_THRESHOLD_US   10000u
#endif

/* Minimum symbols before a frame is considered valid. */
#ifndef APP_IR_RX_MIN_SYMBOLS
#define APP_IR_RX_MIN_SYMBOLS          3u
#endif

/*
 * Frame callback.  Called from the system workqueue when a complete IR
 * frame has been captured.
 *
 *   syms  - captured symbols (mark_us / space_us pairs).
 *   count - number of valid entries; last entry has space_us == 0.
 *   user  - pointer passed to ir_rx_set_callback().
 */
typedef void (*ir_rx_callback_t)(const ir_symbol_t *syms,
                                 size_t count, void *user);

/* One-shot init: configure GPIO input and the free-running timestamp timer.
 * Does NOT start active capture; call ir_rx_start() afterwards. */
int  ir_rx_init(void);
void ir_rx_deinit(void);

/* Register the frame callback.  May be called before or after ir_rx_init(). */
void ir_rx_set_callback(ir_rx_callback_t cb, void *user);

/* Enable / disable edge-interrupt capture. */
int  ir_rx_start(void);
void ir_rx_stop(void);
bool ir_rx_is_active(void);

/*
 * Test / simulation helper: inject a pre-built symbol array as if it had
 * arrived from the physical sensor.  The callback fires synchronously.
 * Useful when APP_IR_RX_NXP_REAL_HW=0 and no hardware is connected.
 */
void ir_rx_inject_frame(const ir_symbol_t *syms, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_RX_H */
