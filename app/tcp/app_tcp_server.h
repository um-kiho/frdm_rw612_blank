/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Single-client TCP listen server on a dedicated FreeRTOS task.
 *
 * Lifecycle:
 *   socket() -> SO_REUSEADDR -> bind(0.0.0.0:port) -> listen() -> accept()
 *   -> blocking recv() loop on the accepted client -> on close, loop back to
 *   accept(). Calling app_tcp_server_stop() tears down both client and
 *   listening sockets.
 *
 * Only one concurrent client is supported. Additional accept() returns are
 * served sequentially after the current client disconnects. Multi-client
 * service would require dispatching each accepted fd to its own task.
 *
 * Must only be started AFTER Wi-Fi is up (lwIP needs a netif with an IP).
 */

#ifndef APP_TCP_SERVER_H
#define APP_TCP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_TCP_SERVER_DEFAULT_PORT
#define APP_TCP_SERVER_DEFAULT_PORT   5000u
#endif

typedef void (*app_tcp_srv_rx_cb_t)(const uint8_t *data, size_t len, void *user_arg);

/* Bind and start listening on the given port. If a server is already
 * running, returns 0 without re-arming. */
int  app_tcp_server_start (uint16_t port,
                           app_tcp_srv_rx_cb_t rx_cb, void *user_arg);

/* Send to the currently connected client. Returns -1 when no client is
 * connected, otherwise the lwip_send() return value. Thread-safe. */
int  app_tcp_server_send  (const void *data, size_t len);

/* Closes any client + the listening socket and stops the task. */
void app_tcp_server_stop  (void);

bool app_tcp_server_is_client_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_TCP_SERVER_H */
