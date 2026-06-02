/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE Central role: scan, connect and subscribe to a sensor-board peripheral
 * that advertises the zCube sensor service.
 *
 * Sensor service UUID base : 5a637562-eXXX-4000-8000-000000000001
 *   e300 : sensor primary service (advertised by the sensor peripheral)
 *   e301 : sensor data characteristic (notify, opaque payload)
 *
 * The RW612 acts as the GATT *client* for the sensor. Inbound notifications
 * are forwarded to the application via the registered rx callback.
 */

#ifndef APP_BLE_CENTRAL_H
#define APP_BLE_CENTRAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum app_central_evt {
    APP_CENTRAL_EVT_SCANNING = 0,
    APP_CENTRAL_EVT_CONNECTED,
    APP_CENTRAL_EVT_SUBSCRIBED,
    APP_CENTRAL_EVT_DISCONNECTED,
    APP_CENTRAL_EVT_ERROR,
} app_central_evt_t;

typedef void (*app_central_evt_cb_t)(app_central_evt_t evt, void *user_arg);
typedef void (*app_central_rx_cb_t)(const uint8_t *data, size_t len, void *user_arg);

/* Initialise + start scanning. Callbacks may be NULL. */
int  app_central_start (app_central_evt_cb_t evt_cb,
                        app_central_rx_cb_t  rx_cb,
                        void *user_arg);

/* Disconnect (if connected), stop scanning. */
void app_central_stop  (void);

bool app_central_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_CENTRAL_H */
