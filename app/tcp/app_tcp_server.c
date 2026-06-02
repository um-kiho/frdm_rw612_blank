/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "app_tcp_server.h"

#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/net/socket.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(tcp_server, LOG_LEVEL_INF);

#ifndef APP_TCP_SRV_RX_BUF_SIZE
#define APP_TCP_SRV_RX_BUF_SIZE   512
#endif
#ifndef APP_TCP_SRV_TASK_STACK
#define APP_TCP_SRV_TASK_STACK    4096
#endif
#ifndef APP_TCP_SRV_TASK_PRIO
#define APP_TCP_SRV_TASK_PRIO     3
#endif
#ifndef APP_TCP_SRV_BACKLOG
#define APP_TCP_SRV_BACKLOG       1
#endif

static int                s_listen_sock = -1;
static int                s_client_sock = -1;
static struct k_mutex     s_lock;
static volatile bool      s_running;
static volatile bool      s_client_connected;
static uint16_t           s_port;
static app_tcp_srv_rx_cb_t s_rx_cb;
static void              *s_rx_arg;

static int build_listening_socket(uint16_t port)
{
    int ls = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) {
        return -1;
    }

    int yes = 1;
    (void)zsock_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port        = htons(port);

    if (zsock_bind(ls, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
        LOG_ERR("bind :%u failed", (unsigned)port);
        zsock_close(ls);
        return -1;
    }
    if (zsock_listen(ls, APP_TCP_SRV_BACKLOG) != 0) {
        LOG_ERR("listen failed");
        zsock_close(ls);
        return -1;
    }
    return ls;
}

static void serve_client(int cs, const struct sockaddr_in *caddr)
{
    static uint8_t rxbuf[APP_TCP_SRV_RX_BUF_SIZE];

    k_mutex_lock(&s_lock, K_FOREVER);
    s_client_sock      = cs;
    s_client_connected = true;
    k_mutex_unlock(&s_lock);

    char addr_str[INET_ADDRSTRLEN];
    zsock_inet_ntop(AF_INET, &caddr->sin_addr, addr_str, sizeof(addr_str));
    LOG_INF("client %s:%u connected", addr_str, (unsigned)ntohs(caddr->sin_port));

    for (;;) {
        int n = zsock_recv(cs, rxbuf, sizeof(rxbuf), 0);
        if (n <= 0) break;
        if (s_rx_cb != NULL) {
            s_rx_cb(rxbuf, (size_t)n, s_rx_arg);
        }
    }

    LOG_INF("client disconnected");

    k_mutex_lock(&s_lock, K_FOREVER);
    s_client_connected = false;
    if (s_client_sock >= 0) {
        zsock_close(s_client_sock);
        s_client_sock = -1;
    }
    k_mutex_unlock(&s_lock);
}

static void srv_task(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;

    while (s_running) {
        int ls = build_listening_socket(s_port);
        if (ls < 0) {
            k_sleep(K_MSEC(1000));
            continue;
        }

        k_mutex_lock(&s_lock, K_FOREVER);
        s_listen_sock = ls;
        k_mutex_unlock(&s_lock);

        LOG_INF("listening on :%u", (unsigned)s_port);

        while (s_running) {
            struct sockaddr_in caddr;
            socklen_t          clen = sizeof(caddr);
            int cs = zsock_accept(ls, (struct sockaddr *)&caddr, &clen);
            if (cs < 0) {
                break;
            }
            serve_client(cs, &caddr);
        }

        k_mutex_lock(&s_lock, K_FOREVER);
        if (s_listen_sock >= 0) {
            zsock_close(s_listen_sock);
            s_listen_sock = -1;
        }
        k_mutex_unlock(&s_lock);

        if (s_running) {
            k_sleep(K_MSEC(500));
        }
    }
}

static K_THREAD_STACK_DEFINE(srv_task_stack, APP_TCP_SRV_TASK_STACK);
static struct k_thread srv_task_data;
static k_tid_t srv_task_tid;

int app_tcp_server_start(uint16_t port,
                         app_tcp_srv_rx_cb_t rx_cb, void *user_arg)
{
    static bool mutex_inited = false;
    
    if (port == 0u) return -1;
    if (s_running)  return 0;

    if (!mutex_inited) {
        k_mutex_init(&s_lock);
        mutex_inited = true;
    }

    s_port    = port;
    s_rx_cb   = rx_cb;
    s_rx_arg  = user_arg;
    s_running = true;

    srv_task_tid = k_thread_create(&srv_task_data, srv_task_stack,
                                    K_THREAD_STACK_SIZEOF(srv_task_stack),
                                    srv_task, NULL, NULL, NULL,
                                    APP_TCP_SRV_TASK_PRIO, 0, K_NO_WAIT);
    if (srv_task_tid == NULL) {
        s_running = false;
        return -3;
    }
    k_thread_name_set(srv_task_tid, "zcube_tcps");
    return 0;
}

int app_tcp_server_send(const void *data, size_t len)
{
    int ret;
    k_mutex_lock(&s_lock, K_FOREVER);
    if (!s_client_connected || s_client_sock < 0) {
        k_mutex_unlock(&s_lock);
        return -1;
    }
    ret = zsock_send(s_client_sock, data, len, 0);
    k_mutex_unlock(&s_lock);
    return ret;
}

void app_tcp_server_stop(void)
{
    s_running = false;
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_client_sock >= 0) {
        zsock_close(s_client_sock);
        s_client_sock      = -1;
        s_client_connected = false;
    }
    if (s_listen_sock >= 0) {
        zsock_close(s_listen_sock);
        s_listen_sock = -1;
    }
    k_mutex_unlock(&s_lock);
}

bool app_tcp_server_is_client_connected(void)
{
    bool c;
    k_mutex_lock(&s_lock, K_FOREVER);
    c = s_client_connected;
    k_mutex_unlock(&s_lock);
    return c;
}
