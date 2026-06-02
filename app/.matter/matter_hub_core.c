/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "matter_hub_core.h"

#include <string.h>

#include <zephyr/logging/log.h>

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
