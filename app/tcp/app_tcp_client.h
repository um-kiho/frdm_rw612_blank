/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * TCP client task. Connects to (host_ip, port) from NVRAM and runs a
 * blocking recv loop on a dedicated FreeRTOS task. Send is callable from
 * any context and is serialised with the task via a mutex.
 *
 * Must only be started AFTER app_wifi reports APP_WIFI_EVT_CONNECTED.
 */

#ifndef APP_TCP_CLIENT_H
#define APP_TCP_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_nvram.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*app_tcp_rx_cb_t)(const uint8_t *data, size_t len, void *user_arg);

int  app_tcp_client_start (const app_nvram_data_t *cfg,
                           app_tcp_rx_cb_t rx_cb, void *user_arg);
int  app_tcp_client_send  (const void *data, size_t len);
void app_tcp_client_stop  (void);
bool app_tcp_client_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_TCP_CLIENT_H */
