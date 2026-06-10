/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Radar sensor UART front-end.
 *
 * Allocation (see develop/frdmrw612_io_interface_spec.md §1):
 *   - Physically a dedicated UART, kept separate from the FRDM debug console.
 *   - Default FLEXCOMM/USART instance: 3  -> USART3  (overridable from build)
 *   - Default line setting:            460800 8N1
 *
 * Frame format is INTENTIONALLY UNSPECIFIED at this stage. The driver
 * forwards each UART RX event's raw bytes directly to the user callback.
 * Once the protocol is locked, plug a framer (FSM / length-prefixed /
 * cobs / ...) inside app_radar_rx_cb_t or add a separate frame_cb on top
 * of this layer.
 *
 * TODO(format):
 *   - Decide SOF/EOF/length/CRC for the radar payload.
 *   - Implement a framer module (e.g. app/radar/app_radar_framer.{h,c}) and
 *     wire it between the raw RX callback and the consumer.
 */

#ifndef APP_RADAR_H
#define APP_RADAR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default port assignment - keep aligned with sdk/frdmrw612/pin_mux.c. */
#ifndef APP_RADAR_UART_INSTANCE
#define APP_RADAR_UART_INSTANCE   3u            /* FLEXCOMM3 / USART3       */
#endif
#ifndef APP_RADAR_UART_BAUDRATE
#define APP_RADAR_UART_BAUDRATE   115200u       /* default line setting */
#endif
#ifndef APP_RADAR_RX_RING_SIZE
#define APP_RADAR_RX_RING_SIZE    512u          /* ring buffer in driver    */
#endif
#ifndef APP_RADAR_RX_CHUNK
#define APP_RADAR_RX_CHUNK        64u           /* per-receive copy size    */
#endif

/* Raw byte stream callback. Called directly from the UART RX event context;
 * keep work short and forward to a queue / framer if heavy processing is
 * needed. */
typedef void (*app_radar_rx_cb_t)(const uint8_t *data, size_t len,
                                  void *user_arg);

int  app_radar_start (app_radar_rx_cb_t rx_cb, void *user_arg);
void app_radar_stop  (void);
bool app_radar_is_running(void);

/* Optional command-out path (radar modules that accept commands). Returns
 * 0 on success, negative on failure. */
int  app_radar_send  (const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* APP_RADAR_H */
