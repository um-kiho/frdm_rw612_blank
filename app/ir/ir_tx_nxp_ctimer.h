/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * NXP RW612 backend for ir_tx.h.
 *
 * Hardware model on RW612 (no dedicated RMT peripheral):
 *
 *   - CTimer N    : 1 MHz tick, two match channels.
 *                   Match 0 fires at the end of the *currently active*
 *                   mark or space, i.e. the boundary between symbols.
 *                   The ISR enables / disables the carrier output and
 *                   loads the next duration into Match 0.
 *
 *   - SCTimer ch  : 38 kHz, 33 % duty, free running on a dedicated pin.
 *                   The carrier is routed to the IR LED driver GPIO and
 *                   gated by toggling the pin's IOPCTL function between
 *                   "SCT output" (carrier on, used during marks) and
 *                   "plain GPIO low" (carrier off, used during spaces).
 *
 * Why CTimer + SCTimer:
 *   - CTimer match interrupts give us microsecond-accurate variable
 *     durations without bit-banging.
 *   - SCTimer can produce a precise 38 kHz duty-controlled PWM with no
 *     CPU intervention, mirroring what the ESP32 RMT carrier engine did.
 *
 * The actual register-level wiring against the NXP SDK is intentionally
 * isolated in this file so callers (encoders, app layer) stay portable.
 * The match/IOPCTL bits flagged "TODO:" below depend on the exact SoC
 * variant + board routing and must be confirmed against the SDK example.
 */

#ifndef APP_IR_TX_NXP_CTIMER_H
#define APP_IR_TX_NXP_CTIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CTimer instance to use as the symbol sequencer. */
#ifndef APP_IR_TX_CTIMER_INSTANCE
#define APP_IR_TX_CTIMER_INSTANCE   1u
#endif

/* SCTimer output channel that produces the 38 kHz carrier. The GPIO pin
 * that physically drives the IR LED gate is muxed to this SCT output
 * during marks and back to a plain GPIO (level=0) during spaces. */
#ifndef APP_IR_TX_SCT_CARRIER_OUTPUT
#define APP_IR_TX_SCT_CARRIER_OUTPUT   0u    /* TODO: confirm SCT_OUT# */
#endif

/* GPIO that drives the IR LED transistor.
 * GPIO26 = HSGPIO0[26] → IO_MUX_SCT_OUT_4 (SCT output 4, 38 kHz carrier)
 * Verified: GPIO26 has SCTIMER_OUT_CLR(4, 1) in RW612-pinctrl.h */
#ifndef APP_IR_TX_GPIO_PORT
#define APP_IR_TX_GPIO_PORT     0u    /* HSGPIO0 */
#endif
#ifndef APP_IR_TX_GPIO_PIN
#define APP_IR_TX_GPIO_PIN      26u   /* GPIO26 */
#endif

/* SCTimer output channel: SCT_OUT4 is mapped to GPIO26 on RW612.
 * IOPCTL alt function for SCT_OUT4 on GPIO26: confirm with SDK pin_mux.c */
#ifndef APP_IR_TX_SCT_CARRIER_OUTPUT
#undef  APP_IR_TX_SCT_CARRIER_OUTPUT
#define APP_IR_TX_SCT_CARRIER_OUTPUT   4u    /* SCT_OUT4 */
#endif

/* IOPCTL FUNC# to swap GPIO26 between SCT output and plain GPIO.
 * SCT alt fn varies by pin — confirm exact value in NXP SDK IOPCTL table. */
#ifndef APP_IR_TX_FUNC_SCT
#define APP_IR_TX_FUNC_SCT      1u    /* SCT_OUT4 alt fn on GPIO26 (verify) */
#endif
#ifndef APP_IR_TX_FUNC_GPIO
#define APP_IR_TX_FUNC_GPIO     0u    /* GPIO fn (default)                  */
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_TX_NXP_CTIMER_H */
