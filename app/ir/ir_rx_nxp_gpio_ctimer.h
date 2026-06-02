/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RW612 IR receive backend configuration.
 *
 * Hardware (from io.txt):
 *   GPIO18 (HSGPIO0[18])  IR Receiver  Input+IRQ  active-low (TSDP341 OUT)
 *
 * The GPIO is driven through the Zephyr DT alias "ir-rx" which maps to the
 * &ir_rx_pin node in boards/frdm_rw612.overlay.  No raw NXP SDK GPIO calls
 * are needed — Zephyr's GPIO driver handles all IOPCTL/IRQ wiring.
 *
 * Timestamp source: k_cycle_get_32() converted to µs.
 * At 200 MHz: 1 cycle = 5 ns → resolution 5 ns, wraparound ~21 s.
 * All frame intervals are < 200 ms, so wraparound is never an issue.
 *
 * CTimer usage: CTIMER1 is reserved for IR TX (ir_tx_nxp_ctimer.c).
 * CTIMER2 and CTIMER3 are available; this backend uses the Zephyr cycle
 * counter instead so no additional CTimer instance is consumed.
 *
 * Gap / end-of-frame:
 *   A k_work_delayable is re-armed on every edge.  When it fires after
 *   APP_IR_RX_GAP_WORK_MS of silence the frame is complete.
 */

#ifndef APP_IR_RX_NXP_GPIO_CTIMER_H
#define APP_IR_RX_NXP_GPIO_CTIMER_H

#include "ir_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zephyr DT alias for the IR receiver GPIO.
 * Defined in boards/frdm_rw612.overlay:
 *
 *   / {
 *     aliases { ir-rx = &ir_rx_pin; };
 *     ir_rx_pin: ir-receiver {
 *       compatible = "gpio-keys";
 *       ir-rx-gpio {
 *         gpios = <&gpio0 18 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
 *         label = "IR_RX";
 *       };
 *     };
 *   };
 *
 * The 'gpios' property drives gpio_pin_configure_dt() automatically.
 */
#define APP_IR_RX_DT_ALIAS   ir_rx   /* DT_ALIAS(ir_rx) → &ir_rx_pin */

/* Workqueue delay after the last detected edge (ms).
 * Must exceed APP_IR_RX_GAP_THRESHOLD_US / 1000 (10 ms) with margin. */
#ifndef APP_IR_RX_GAP_WORK_MS
#define APP_IR_RX_GAP_WORK_MS   12u
#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_RX_NXP_GPIO_CTIMER_H */
