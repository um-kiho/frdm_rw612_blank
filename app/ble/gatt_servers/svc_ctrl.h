/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE control GATT service (server #3).
 *
 * Phone writes a 1-byte opcode (optional payload follows) to "cmd". The
 * service delivers the command to app_main via the registered callback,
 * then notifies the result on "resp".
 *
 * Service UUID base : 5a637562-eXXX-4000-8000-000000000001
 *   e200 : service
 *   e201 : cmd  (write,  opcode byte + optional payload)
 *   e202 : resp (read + notify, 1 byte status)
 */

#ifndef APP_BLE_SVC_CTRL_H
#define APP_BLE_SVC_CTRL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opcodes received via the "cmd" characteristic. */
#define APP_CTRL_OP_REBOOT             0x01u
#define APP_CTRL_OP_FACTORY_RESET      0x02u
#define APP_CTRL_OP_WIFI_DISCONNECT    0x03u
#define APP_CTRL_OP_WIFI_RECONNECT     0x04u
#define APP_CTRL_OP_TCP_DISCONNECT     0x05u
#define APP_CTRL_OP_TCP_RECONNECT      0x06u

/* Response status bytes pushed to the "resp" characteristic. */
#define APP_CTRL_RSP_OK                0x00u
#define APP_CTRL_RSP_UNKNOWN_OP        0x01u
#define APP_CTRL_RSP_INVALID_STATE     0x02u
#define APP_CTRL_RSP_REJECTED          0x03u

typedef enum app_ctrl_evt {
    APP_CTRL_EVT_REBOOT = 0,
    APP_CTRL_EVT_FACTORY_RESET,
    APP_CTRL_EVT_WIFI_DISCONNECT,
    APP_CTRL_EVT_WIFI_RECONNECT,
    APP_CTRL_EVT_TCP_DISCONNECT,
    APP_CTRL_EVT_TCP_RECONNECT,
} app_ctrl_evt_t;

typedef void (*app_ctrl_evt_cb_t)(app_ctrl_evt_t evt, void *user_arg);

int  app_ctrl_init    (app_ctrl_evt_cb_t cb, void *user_arg);
void app_ctrl_respond (uint8_t status);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_SVC_CTRL_H */
