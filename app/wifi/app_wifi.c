/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Wi-Fi STA wrapper using Zephyr WiFi Management API
 *
 * NXP RW612 드라이버는 NET_EVENT_IF_UP을 발행하지 않으므로
 * k_work_delayable(최대 5초)로 READY를 발행한 뒤 connect를 시도한다.
 * CONFIG_HEAP_MEM_POOL_SIZE=153600 이상이어야 펌웨어 로딩이 완료된다.
 *
 * WIFI_FREQ_BAND_UNKNOWN은 NXP 드라이버에서 AP 미발견 시 CONNECT_RESULT를
 * 발행하지 않으므로 사용하지 않는다.
 * 5GHz → 2.4GHz 순으로 명시적 재시도하며, 각 시도마다 35초 타임아웃을
 * k_work_delayable로 강제 적용한다.
 */

#include "app_wifi.h"

#include <string.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_wifi, LOG_LEVEL_INF);

static app_wifi_evt_cb_t s_cb;
static void             *s_cb_arg;
static volatile bool     s_started;
static volatile bool     s_connected;
static struct net_if    *s_iface;

static struct net_mgmt_event_callback s_wifi_cb;
static struct net_mgmt_event_callback s_ipv4_cb;
static struct net_mgmt_event_callback s_iface_cb;

static struct k_work_delayable s_ready_dwork;

static int s_connected_band;

/* Band retry sequence: 5GHz first, then 2.4GHz */
static const enum wifi_frequency_bands k_band_seq[] = {
    WIFI_FREQ_BAND_5_GHZ,
    WIFI_FREQ_BAND_2_4_GHZ,
};
#define BAND_SEQ_LEN    ARRAY_SIZE(k_band_seq)
#define CONNECT_TIMEOUT_S  35   /* NXP 드라이버 connect 최대 대기 시간 */

static int              s_retry_idx;
static app_nvram_data_t s_retry_cfg;
static struct k_work    s_retry_work;
static struct k_work_delayable s_connect_timeout_dwork;

static void emit(app_wifi_evt_t evt)
{
    if (s_cb != NULL) {
        s_cb(evt, s_cb_arg);
    }
}

static void start_connect_timeout(void)
{
    k_work_schedule(&s_connect_timeout_dwork, K_SECONDS(CONNECT_TIMEOUT_S));
}

static void cancel_connect_timeout(void)
{
    k_work_cancel_delayable(&s_connect_timeout_dwork);
}

static void connect_timeout_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    LOG_WRN("Connect timeout band_idx=%d — trying next band", s_retry_idx);
    /* Force disconnect to reset driver state, then retry */
    net_mgmt(NET_REQUEST_WIFI_DISCONNECT, s_iface, NULL, 0);
    s_retry_idx++;
    k_work_submit(&s_retry_work);
}

static void retry_work_fn(struct k_work *w)
{
    ARG_UNUSED(w);

    if (s_retry_idx >= (int)BAND_SEQ_LEN) {
        LOG_ERR("All band retries exhausted");
        emit(APP_WIFI_EVT_AUTH_FAILED);
        return;
    }

    enum wifi_frequency_bands band = k_band_seq[s_retry_idx];
    const char *band_str = (band == WIFI_FREQ_BAND_5_GHZ) ? "5GHz" : "2.4GHz";

    struct wifi_connect_req_params params;
    memset(&params, 0, sizeof(params));
    params.ssid        = (uint8_t *)s_retry_cfg.ssid;
    params.ssid_length = strlen(s_retry_cfg.ssid);
    params.band        = band;
    params.channel     = WIFI_CHANNEL_ANY;
    params.timeout     = CONNECT_TIMEOUT_S * 1000;

    if (strcmp(s_retry_cfg.security, "OPEN") == 0) {
        params.security = WIFI_SECURITY_TYPE_NONE;
    } else if (strcmp(s_retry_cfg.security, "WPA2") == 0) {
        params.security   = WIFI_SECURITY_TYPE_PSK;
        params.psk        = (uint8_t *)s_retry_cfg.password;
        params.psk_length = strlen(s_retry_cfg.password);
    } else if (strcmp(s_retry_cfg.security, "WPA2_WPA3") == 0 ||
               strcmp(s_retry_cfg.security, "WPA3_SAE") == 0) {
        params.security   = WIFI_SECURITY_TYPE_SAE;
        params.psk        = (uint8_t *)s_retry_cfg.password;
        params.psk_length = strlen(s_retry_cfg.password);
    } else {
        LOG_ERR("Unsupported security: %s", s_retry_cfg.security);
        emit(APP_WIFI_EVT_AUTH_FAILED);
        return;
    }

    LOG_INF("Connecting to '%s' (%s) band=%s [attempt %d/%d]",
            s_retry_cfg.ssid, s_retry_cfg.security, band_str,
            s_retry_idx + 1, (int)BAND_SEQ_LEN);

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, s_iface, &params, sizeof(params));
    if (ret != 0) {
        LOG_ERR("connect request failed ret=%d, trying next band", ret);
        s_retry_idx++;
        k_work_submit(&s_retry_work);
        return;
    }

    start_connect_timeout();
}

/* 대량 전송(OTA 등) 중 WiFi 파워세이브가 켜져 있으면 AP가 버퍼링→DTIM 주기로
 * 묶어 보내며 수십~수백 ms 단위 정지가 발생한다. 연결 직후 PS를 끈다. */
static void disable_power_save(void)
{
    struct wifi_ps_params params = { 0 };
    params.type    = WIFI_PS_PARAM_STATE;
    params.enabled = WIFI_PS_DISABLED;

    int ret = net_mgmt(NET_REQUEST_WIFI_PS, s_iface, &params, sizeof(params));
    if (ret != 0) {
        LOG_WRN("Failed to disable WiFi power save (ret=%d)", ret);
    } else {
        LOG_INF("WiFi power save disabled");
    }
}

static void log_connection_status(void)
{
    struct wifi_iface_status st = { 0 };

    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, s_iface, &st, sizeof(st)) != 0) {
        LOG_WRN("Failed to get WiFi status");
        return;
    }

    s_connected_band = st.band;

    const char *band_str =
        (st.band == WIFI_FREQ_BAND_2_4_GHZ) ? "2.4GHz" :
        (st.band == WIFI_FREQ_BAND_5_GHZ)   ? "5GHz"   :
        (st.band == WIFI_FREQ_BAND_6_GHZ)   ? "6GHz"   : "unknown";

    LOG_INF("Connected: band=%s channel=%u rssi=%d dBm ssid=[%s]",
            band_str, st.channel, st.rssi, st.ssid);
}

/* NXP WiFi 펌웨어 로딩 완료 대기 후 READY 발행 (3초 fallback 또는 IF_UP 즉시) */
static void ready_work_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    if (s_started) {
        return; /* IF_UP로 이미 처리됨 */
    }
    LOG_INF("WiFi driver ready (fallback timer)");
    s_started = true;
    emit(APP_WIFI_EVT_READY);
}

/* NET_EVENT_IF_UP: 드라이버가 3초 이내에 준비되면 즉시 READY 발행 */
static void iface_event_handler(struct net_mgmt_event_callback *cb,
                                uint64_t mgmt_event, struct net_if *iface)
{
    (void)cb;
    if (iface != s_iface) {
        return;
    }
    if (mgmt_event == NET_EVENT_IF_UP && !s_started) {
        LOG_INF("WiFi driver ready (NET_EVENT_IF_UP)");
        k_work_cancel_delayable(&s_ready_dwork); /* fallback 타이머 취소 */
        s_started = true;
        emit(APP_WIFI_EVT_READY);
    }
}

/* WiFi 연결/해제/스캔 결과 이벤트 */
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                    uint64_t mgmt_event, struct net_if *iface)
{
    (void)cb;
    (void)iface;

    switch (mgmt_event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *ws =
            (const struct wifi_status *)cb->info;
        if (ws == NULL || ws->conn_status != WIFI_STATUS_CONN_SUCCESS) {
            int st = (ws != NULL) ? (int)ws->conn_status : -1;
            LOG_WRN("Connect failed status=%d (%s) band_idx=%d", st,
                    (st == (int)WIFI_STATUS_CONN_WRONG_PASSWORD) ? "wrong password" :
                    (st == (int)WIFI_STATUS_CONN_TIMEOUT)         ? "timeout"        :
                    (st == (int)WIFI_STATUS_CONN_AP_NOT_FOUND)    ? "AP not found"   :
                    "other", s_retry_idx);

            cancel_connect_timeout();

            /* Wrong password: band retry won't help */
            if (st == (int)WIFI_STATUS_CONN_WRONG_PASSWORD) {
                emit(APP_WIFI_EVT_AUTH_FAILED);
            } else {
                s_retry_idx++;
                k_work_submit(&s_retry_work);
            }
        } else {
            cancel_connect_timeout();
            LOG_INF("WiFi connected");
            s_connected = true;
            disable_power_save();
            log_connection_status();
        }
        break;
    }

    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        if (s_connected) {
            LOG_INF("WiFi disconnected");
            s_connected = false;
            s_connected_band = 0;
            emit(APP_WIFI_EVT_DISCONNECTED);
        }
        break;

    case NET_EVENT_WIFI_SCAN_DONE:
        LOG_INF("WiFi scan done");
        break;

    default:
        break;
    }
}

/* DHCP IP 획득 이벤트 */
static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event, struct net_if *iface)
{
    (void)cb;
    (void)iface;

    if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
        LOG_INF("DHCP bound");
        emit(APP_WIFI_EVT_CONNECTED);
    }
}

int app_wifi_init(app_wifi_evt_cb_t cb, void *user_arg)
{
    s_cb        = cb;
    s_cb_arg    = user_arg;
    s_started   = false;
    s_connected = false;

    s_iface = net_if_get_first_wifi();
    if (s_iface == NULL) {
        LOG_ERR("No WiFi interface found");
        return -1;
    }

    k_work_init(&s_retry_work, retry_work_fn);
    k_work_init_delayable(&s_connect_timeout_dwork, connect_timeout_fn);

    net_mgmt_init_event_callback(&s_wifi_cb, wifi_mgmt_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT |
                                 NET_EVENT_WIFI_SCAN_DONE);
    net_mgmt_add_event_callback(&s_wifi_cb);

    net_mgmt_init_event_callback(&s_ipv4_cb, ipv4_event_handler,
                                 NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&s_ipv4_cb);

    /* IF_UP: 드라이버가 3초 이내에 준비되면 즉시 READY 발행 */
    net_mgmt_init_event_callback(&s_iface_cb, iface_event_handler,
                                 NET_EVENT_IF_UP);
    net_mgmt_add_event_callback(&s_iface_cb);

    net_if_up(s_iface);

    k_work_init_delayable(&s_ready_dwork, ready_work_fn);
    k_work_schedule(&s_ready_dwork, K_SECONDS(5));

    LOG_INF("WiFi initialized — ready in ~5s");
    return 0;
}

int app_wifi_connect(const app_nvram_data_t *cfg)
{
    if (cfg == NULL)      return -1;
    if (!s_started)       return -2;
    if (s_iface == NULL)  return -3;

    cancel_connect_timeout();
    s_retry_cfg = *cfg;
    s_retry_idx = 0;
    k_work_submit(&s_retry_work);
    return 0;
}

int app_wifi_disconnect(void)
{
    if (s_iface == NULL) return -1;
    cancel_connect_timeout();
    s_retry_idx = (int)BAND_SEQ_LEN; /* cancel pending retry */
    s_connected = false;
    return (net_mgmt(NET_REQUEST_WIFI_DISCONNECT, s_iface, NULL, 0) == 0) ? 0 : -1;
}

bool app_wifi_is_connected(void)
{
    return s_connected;
}

int app_wifi_get_band(void)
{
    return s_connected_band;
}
