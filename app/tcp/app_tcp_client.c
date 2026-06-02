/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_tcp_client.h"

#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/net/socket.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tcp_client, LOG_LEVEL_INF);

#ifndef APP_TCP_RX_BUF_SIZE
#define APP_TCP_RX_BUF_SIZE   512
#endif
#ifndef APP_TCP_TASK_STACK
#define APP_TCP_TASK_STACK    4096
#endif
#ifndef APP_TCP_TASK_PRIO
#define APP_TCP_TASK_PRIO     3
#endif

#define APP_TCP_RETRY_MS_MIN  500u
#define APP_TCP_RETRY_MS_MAX  10000u

static int                s_sock = -1;
static struct k_mutex     s_lock;
static volatile bool      s_running;
static volatile bool      s_connected;
static app_nvram_data_t   s_cfg;
static app_tcp_rx_cb_t    s_rx_cb;
static void              *s_rx_arg;

static uint32_t backoff_next(uint32_t cur)
{
    uint32_t n = cur * 2u;
    return (n > APP_TCP_RETRY_MS_MAX) ? APP_TCP_RETRY_MS_MAX : n;
}

static void tcp_task(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;
    static uint8_t rxbuf[APP_TCP_RX_BUF_SIZE];
    uint32_t       backoff = APP_TCP_RETRY_MS_MIN;

    while (s_running) {
        int s = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) {
            k_sleep(K_MSEC(backoff));
            backoff = backoff_next(backoff);
            continue;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(s_cfg.port);
        zsock_inet_pton(AF_INET, s_cfg.host_ip, &addr.sin_addr);

        if (zsock_connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            LOG_ERR("connect %s:%u failed", s_cfg.host_ip, (unsigned)s_cfg.port);
            zsock_close(s);
            k_sleep(K_MSEC(backoff));
            backoff = backoff_next(backoff);
            continue;
        }
        backoff = APP_TCP_RETRY_MS_MIN;

        k_mutex_lock(&s_lock, K_FOREVER);
        s_sock      = s;
        s_connected = true;
        k_mutex_unlock(&s_lock);

        LOG_INF("connected %s:%u", s_cfg.host_ip, (unsigned)s_cfg.port);

        for (;;) {
            int n = zsock_recv(s, rxbuf, sizeof(rxbuf), 0);
            if (n <= 0) break;
            if (s_rx_cb != NULL) {
                s_rx_cb(rxbuf, (size_t)n, s_rx_arg);
            }
        }

        LOG_INF("disconnected");
        k_mutex_lock(&s_lock, K_FOREVER);
        s_connected = false;
        s_sock      = -1;
        k_mutex_unlock(&s_lock);
        zsock_close(s);
    }
}

static K_THREAD_STACK_DEFINE(tcp_task_stack, APP_TCP_TASK_STACK);
static struct k_thread tcp_task_data;
static k_tid_t tcp_task_tid;

int app_tcp_client_start(const app_nvram_data_t *cfg,
                         app_tcp_rx_cb_t rx_cb, void *user_arg)
{
    static bool mutex_inited = false;
    
    if (cfg == NULL) return -1;
    if (s_running)   return 0;

    if (!mutex_inited) {
        k_mutex_init(&s_lock);
        mutex_inited = true;
    }

    s_cfg     = *cfg;
    s_rx_cb   = rx_cb;
    s_rx_arg  = user_arg;
    s_running = true;

    tcp_task_tid = k_thread_create(&tcp_task_data, tcp_task_stack,
                                    K_THREAD_STACK_SIZEOF(tcp_task_stack),
                                    tcp_task, NULL, NULL, NULL,
                                    APP_TCP_TASK_PRIO, 0, K_NO_WAIT);
    if (tcp_task_tid == NULL) {
        s_running = false;
        return -3;
    }
    k_thread_name_set(tcp_task_tid, "zcube_tcp");
    return 0;
}

int app_tcp_client_send(const void *data, size_t len)
{
    int ret;
    k_mutex_lock(&s_lock, K_FOREVER);
    if (!s_connected || s_sock < 0) {
        k_mutex_unlock(&s_lock);
        return -1;
    }
    ret = zsock_send(s_sock, data, len, 0);
    k_mutex_unlock(&s_lock);
    return ret;
}

void app_tcp_client_stop(void)
{
    s_running = false;
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_sock >= 0) {
        zsock_close(s_sock);
        s_sock      = -1;
        s_connected = false;
    }
    k_mutex_unlock(&s_lock);
}

bool app_tcp_client_is_connected(void)
{
    bool c;
    k_mutex_lock(&s_lock, K_FOREVER);
    c = s_connected;
    k_mutex_unlock(&s_lock);
    return c;
}
