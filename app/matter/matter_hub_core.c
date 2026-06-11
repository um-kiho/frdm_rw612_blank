/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "matter_hub_core.h"

#include <string.h>

#include <zephyr/logging/log.h>

#if defined(CONFIG_NET_L2_OPENTHREAD)
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#endif

LOG_MODULE_REGISTER(app_matter_hub, LOG_LEVEL_INF);

#ifndef MATTER_MAX_FABRIC_COUNT
#define MATTER_MAX_FABRIC_COUNT  8u
#endif

static matter_prefs_payload_t s_prefs;
static bool                   s_inited;
static bool                   s_window_active;
static uint32_t               s_window_end_uptime_s;
static uint32_t               s_cached_uptime_s;

static int persist_prefs(void)
{
    return matter_prefs_save(&s_prefs);
}

#if defined(CONFIG_NET_L2_OPENTHREAD) && defined(CONFIG_APP_MATTER_THREAD_AUTOSTART)
/* ── OpenThread (Thread RCP host) ──────────────────────────────────────────
 * RW612 온칩 802.15.4 라디오를 NBU RCP 펌웨어로 구동하고, 호스트(M33)가 내부
 * HDLC 인터페이스로 OpenThread 풀스택을 돌린다. CONFIG_OPENTHREAD_MANUAL_START
 * 때문에 SYS_INIT 단계에서는 openthread_init()(인스턴스 생성 + RCP 기동)만 되고
 * 네트워크는 올라오지 않는다. 여기서 openthread_run()으로 IPv6+Thread 를 기동한다
 * (저장된 데이터셋이 있으면 재사용, 없으면 Kconfig 기본 네트워크로 form). */
static const char *ot_role_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED: return "disabled";
    case OT_DEVICE_ROLE_DETACHED: return "detached";
    case OT_DEVICE_ROLE_CHILD:    return "child";
    case OT_DEVICE_ROLE_ROUTER:   return "router";
    case OT_DEVICE_ROLE_LEADER:   return "leader";
    default:                      return "unknown";
    }
}

/* OT 워크큐 컨텍스트에서 호출됨(이미 OT 뮤텍스 보유) → API 직접 호출 안전 */
static void ot_state_changed(otChangedFlags flags, void *ctx)
{
    (void)ctx;
    if ((flags & OT_CHANGED_THREAD_ROLE) != 0u) {
        otInstance *inst = openthread_get_default_instance();
        if (inst != NULL) {
            LOG_INF("Thread role -> %s", ot_role_str(otThreadGetDeviceRole(inst)));
        }
    }
}

static struct openthread_state_changed_callback s_ot_cb = {
    .otCallback = ot_state_changed,
    .user_data  = NULL,
};

static void thread_start(void)
{
    int rc = openthread_state_changed_callback_register(&s_ot_cb);
    if (rc != 0) {
        LOG_WRN("OT state cb register rc=%d", rc);
    }

    rc = openthread_run();      /* IPv6 + Thread 기동 (manual-start 경로) */
    if (rc != 0) {
        LOG_ERR("openthread_run rc=%d — Thread netif not up", rc);
        return;
    }
    LOG_INF("OpenThread started (RCP host, FTD)");
}
#endif /* CONFIG_NET_L2_OPENTHREAD */

int matter_hub_init(void)
{
    if (s_inited) {
        return 0;
    }

    int rc = matter_prefs_load(&s_prefs);
    /* Flash 비어있거나 깨졌든: 기본 채워진 상태를 저장하여 슬롯을 확보 */
    if (rc != 0) {
        LOG_WRN("prefs load rc=%d, writing defaults", rc);
        if (matter_prefs_save(&s_prefs) != 0) {
            return -2;
        }
    }

    s_window_end_uptime_s   = 0u;
    s_inited                = true;

    matter_device_descriptor_t d;
    matter_hub_get_descriptor(&d);
    LOG_INF("ready VID=%u PID=%u name=%s fabric=%u state=%lu",
        (unsigned)d.vendor_id, (unsigned)d.product_id, d.friendly_name,
        (unsigned)s_prefs.fabric_count, (unsigned long)s_prefs.commissioning_state);

#if defined(CONFIG_NET_L2_OPENTHREAD) && defined(CONFIG_APP_MATTER_THREAD_AUTOSTART)
    /* Matter 백본: Thread(OpenThread RCP host) 기동. WiFi 부팅을 방해하지 않도록
     * MANUAL_START 로 두고 여기서 명시적으로 올린다.
     * CONFIG_APP_MATTER_THREAD_AUTOSTART=n 이면 생략 → 15.4 idle(coex 진단용). */
    thread_start();
#elif defined(CONFIG_NET_L2_OPENTHREAD)
    LOG_INF("Thread autostart disabled (OT idle) — coex diagnostic");
#endif

    return 0;
}

void matter_hub_on_tick(uint32_t uptime_s)
{
    if (!s_inited) {
        return;
    }
    s_cached_uptime_s = uptime_s;

    if (!s_window_active || s_window_end_uptime_s == 0u) {
        return;
    }
    if (uptime_s < s_window_end_uptime_s) {
        return;
    }

    s_window_active           = false;
    s_window_end_uptime_s     = 0u;
    LOG_INF("commissioning window closed (timeout)");
}

void matter_hub_get_descriptor(matter_device_descriptor_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->vendor_id       = APP_MATTER_VENDOR_ID;
    out->product_id      = APP_MATTER_PRODUCT_ID;
    out->hardware_version = (uint16_t)APP_MATTER_HARDWARE_VERSION;
    out->software_version = APP_MATTER_SOFTWARE_VERSION;
    strncpy(out->friendly_name, APP_MATTER_DEVICE_NAME, sizeof(out->friendly_name) - 1u);
}

void matter_hub_get_prefs_view(matter_prefs_payload_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_prefs;
    if (s_window_active) {
        out->commissioning_state = (uint32_t)MATTER_CMP_WINDOW_OPEN;
    }
}

int matter_hub_commission_window_open(uint32_t duration_s, uint32_t uptime_s)
{
    if (!s_inited) {
        return -2;
    }
    if (duration_s == 0u) {
        duration_s = APP_MATTER_DEFAULT_COMMISSION_SECONDS;
    }

    /* 윈도우는 RAM 만 — 플래시에는 READY/COMMISSIONED 만 유지 (재부팅 시 유실됨은 스텁 한계) */
    s_cached_uptime_s       = uptime_s;
    s_window_active         = true;
    s_window_end_uptime_s   = uptime_s + duration_s;
    LOG_INF("commissioning window until uptime=%lu",
            (unsigned long)s_window_end_uptime_s);
    return 0;
}

int matter_hub_commission_window_close(void)
{
    if (!s_inited) {
        return -2;
    }
    s_window_active           = false;
    s_window_end_uptime_s     = 0u;
    return 0;
}

int matter_hub_factory_reset(void)
{
    memset(&s_prefs, 0, sizeof(s_prefs));
    s_prefs.commissioning_state = (uint32_t)MATTER_CMP_READY;
    s_window_active             = false;
    s_window_end_uptime_s       = 0u;
    int rc                      = persist_prefs();
    LOG_INF("factory reset Matter prefs (%d)", rc);
    return rc;
}

int matter_hub_dev_simulate_fabric_commissioned(void)
{
    if (!s_inited) {
        return -2;
    }
    if (s_prefs.fabric_count < MATTER_MAX_FABRIC_COUNT) {
        s_prefs.fabric_count++;
    }
    s_prefs.commissioning_state = (uint32_t)MATTER_CMP_COMMISSIONED;
    s_window_active             = false;
    s_window_end_uptime_s       = 0u;
    LOG_INF("simulate fabric #%u commissioned",
            (unsigned)s_prefs.fabric_count);
    return persist_prefs();
}

uint32_t matter_hub_commission_window_remaining_s(uint32_t uptime_s)
{
    if (!s_window_active) {
        return 0u;
    }
    if (s_window_end_uptime_s <= uptime_s) {
        return 0u;
    }
    return s_window_end_uptime_s - uptime_s;
}

uint32_t matter_hub_cached_uptime_s(void)
{
    return s_cached_uptime_s;
}

void matter_hub_pack_status(uint8_t buf[MATTER_HUB_STATUS_PACKED_LEN])
{
    memset(buf, 0, MATTER_HUB_STATUS_PACKED_LEN);

    matter_prefs_payload_t vw;
    matter_hub_get_prefs_view(&vw);

    buf[0]               = (uint8_t)(vw.commissioning_state & 0xFFu);
    buf[1]               = vw.fabric_count;
    uint16_t wr          = (uint16_t)matter_hub_commission_window_remaining_s(s_cached_uptime_s);
    buf[2]               = (uint8_t)(wr & 0xFFu);
    buf[3]               = (uint8_t)((wr >> 8) & 0xFFu);

    matter_device_descriptor_t d;
    matter_hub_get_descriptor(&d);

    buf[4]               = (uint8_t)(d.vendor_id & 0xFFu);
    buf[5]               = (uint8_t)((d.vendor_id >> 8) & 0xFFu);
    buf[6]               = (uint8_t)(d.product_id & 0xFFu);
    buf[7]               = (uint8_t)((d.product_id >> 8) & 0xFFu);
    buf[8]               = (uint8_t)(d.hardware_version & 0xFFu);
    buf[9]               = (uint8_t)((d.hardware_version >> 8) & 0xFFu);
    buf[10]              = (uint8_t)(d.software_version & 0xFFu);
    buf[11]              = (uint8_t)((d.software_version >> 8) & 0xFFu);
}
