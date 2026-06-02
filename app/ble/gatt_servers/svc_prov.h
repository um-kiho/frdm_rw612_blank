/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE provisioning GATT service.
 *
 * Phone app writes the five fields { ssid, password, security, host_ip, port }
 * one characteristic at a time into the device-side staging buffer, then writes
 * to the "commit" characteristic to ask the device to validate and persist them
 * via the NVRAM layer (apps/zcube/app/nvm/app_nvram.h).
 *
 * The "state" characteristic exposes the latest commit outcome and can be
 * subscribed to for notifications.
 */

#ifndef APP_BLE_SVC_PROV_H
#define APP_BLE_SVC_PROV_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum app_prov_evt {
    APP_PROV_EVT_NONE = 0,
    APP_PROV_EVT_COMMIT_OK,        /* validated + persisted to NVRAM */
    APP_PROV_EVT_COMMIT_INVALID,   /* validation failed              */
    APP_PROV_EVT_COMMIT_IO_ERROR,  /* mflash write failed            */
    APP_PROV_EVT_RESET,            /* host requested factory reset   */
} app_prov_evt_t;

/* State byte exposed via the "state" characteristic (read + notify). */
#define APP_PROV_STATE_IDLE         0u
#define APP_PROV_STATE_COMMITTED    1u
#define APP_PROV_STATE_INVALID      2u
#define APP_PROV_STATE_IO_ERROR     3u
#define APP_PROV_STATE_APPLIED      4u   /* set by app_main after Wi-Fi/TCP OK */

/* Commit-characteristic opcodes (first byte of the write). */
#define APP_PROV_OP_COMMIT          0x01u
#define APP_PROV_OP_RESET           0xFFu

typedef void (*app_prov_evt_cb_t)(app_prov_evt_t evt, void *user_arg);

int  app_prov_init(app_prov_evt_cb_t cb, void *user_arg);

/* Push a new state byte and notify subscribers (no-op if nobody subscribed). */
void app_prov_set_state(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_SVC_PROV_H */
