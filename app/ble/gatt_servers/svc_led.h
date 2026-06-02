/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE LED control GATT service.
 *
 * Service UUID base : 5a637562-eXXX-4000-8000-000000000001
 *   e700 : service
 *   e701 : cmd  (write,  opcode byte + payload)
 *   e702 : status (read + notify, packed state report)
 *
 * Opcodes - phone writes 1 byte opcode then opcode-specific payload:
 *
 *   0x01 SET_PATTERN   : 1 byte led_pattern (LED_PAT_*)
 *   0x02 SET_COLOR     : 4 bytes  R, G, B, W   (base colour, big-endian)
 *   0x03 SET_BRIGHTNESS: 1 byte  0..255 q8 scaler
 *   0x04 OFF           : -                     (shorthand for pat=OFF)
 *   0x05 SET_PIXEL     : 2 bytes idx LE, 4 bytes R G B W  (SOLID mode only)
 *   0x06 SET_PATTERN_COLOR : 1 byte pattern + 4 bytes R G B W (combined op)
 *   0x07 SET_SLEEP_LIGHT   : 1 byte preset (sleep_light_preset_t) + 1 byte
 *                            brightness_pct (0..100). Bedtime preset LUT;
 *                            resolves to LED_PAT_SOLID with the LUT colour.
 *
 * Status (e702) layout (8 bytes, packed LE):
 *   [0] pattern   (led_pattern_t)
 *   [1] R
 *   [2] G
 *   [3] B
 *   [4] W
 *   [5] brightness
 *   [6] led_count (LSB)
 *   [7] led_count (MSB)
 */

#ifndef APP_BLE_SVC_LED_H
#define APP_BLE_SVC_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_LED_OP_SET_PATTERN          0x01u
#define APP_LED_OP_SET_COLOR            0x02u
#define APP_LED_OP_SET_BRIGHTNESS       0x03u
#define APP_LED_OP_OFF                  0x04u
#define APP_LED_OP_SET_PIXEL            0x05u
#define APP_LED_OP_SET_PATTERN_COLOR    0x06u
#define APP_LED_OP_SET_SLEEP_LIGHT      0x07u

/* Registers the e702 status notification. Must be called once after the
 * BLE controller is up (typically from bt_ready_cb()). */
int app_led_svc_init(void);

/* Refresh the cached status struct from led_task + sk6812 state and push
 * it to any subscriber via bt_gatt_notify(). Safe to call from any task. */
void app_led_svc_publish_status(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_SVC_LED_H */
