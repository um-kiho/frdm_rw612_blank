/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Wi-Fi STA wrapper around NXP wlan_* API. Initialised once at boot, then
 * driven by app_main using a record loaded from NVRAM.
 */

#ifndef APP_WIFI_H
#define APP_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include "app_nvram.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum app_wifi_evt {
    APP_WIFI_EVT_READY = 0,        /* wlan_start has completed */
    APP_WIFI_EVT_CONNECTED,        /* STA joined AP, DHCP done */
    APP_WIFI_EVT_DISCONNECTED,
    APP_WIFI_EVT_AUTH_FAILED,
    APP_WIFI_EVT_INIT_FAILED,
} app_wifi_evt_t;

typedef void (*app_wifi_evt_cb_t)(app_wifi_evt_t evt, void *user_arg);

/* Brings up the wlan driver and registers the event callback. */
int  app_wifi_init      (app_wifi_evt_cb_t cb, void *user_arg);

/* Builds a wlan_network from cfg and calls wlan_connect. Result is reported
 * asynchronously via the callback registered in app_wifi_init. */
int  app_wifi_connect   (const app_nvram_data_t *cfg);

int  app_wifi_disconnect(void);
bool app_wifi_is_connected(void);

/* Returns the connected band: 0=unknown, 1=2.4GHz, 2=5GHz.
 * Valid only while app_wifi_is_connected() is true. */
int  app_wifi_get_band(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_WIFI_H */
