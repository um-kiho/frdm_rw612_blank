/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "svc_led.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>

#include "led_task.h"
#include "sk6812.h"
#include "sleep_light_table.h"

LOG_MODULE_REGISTER(app_svc_led, LOG_LEVEL_INF);

/* app_main exposes this so the state-machine LED policy backs off while
 * the phone is in control. Implemented in app_main.c. */
extern void app_main_led_manual_override(uint32_t hold_s);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562U, (eXXX), 0x4000U, 0x8000U, 0x000000000001ULL)

#define BT_UUID_LED_SVC_VAL     UUID_BASE(0xe700)
#define BT_UUID_LED_CMD_VAL     UUID_BASE(0xe701)
#define BT_UUID_LED_STA_VAL     UUID_BASE(0xe702)

static struct bt_uuid_128 uuid_led_svc = BT_UUID_INIT_128(BT_UUID_LED_SVC_VAL);
static struct bt_uuid_128 uuid_led_cmd = BT_UUID_INIT_128(BT_UUID_LED_CMD_VAL);
static struct bt_uuid_128 uuid_led_sta = BT_UUID_INIT_128(BT_UUID_LED_STA_VAL);

/* Status struct mirrors the wire layout described in svc_led.h. */
typedef struct __attribute__((packed)) {
    uint8_t  pattern;
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  w;
    uint8_t  brightness;
    uint16_t led_count;
} led_status_t;

static led_status_t s_status;

/* Attribute layout for BT_GATT_SERVICE_DEFINE below:
 *   0: primary service
 *   1: char decl (cmd)
 *   2: char value (cmd)
 *   3: char decl (status)
 *   4: char value (status)     <-- ATTR_IDX_STA_VAL
 *   5: CCC (status)
 */
#define ATTR_IDX_STA_VAL  4

static void refresh_cached_status(void)
{
    led_pattern_t  p = led_task_get_pattern();
    sk6812_color_t c = led_task_get_color();
    s_status.pattern    = (uint8_t)p;
    s_status.r          = c.r;
    s_status.g          = c.g;
    s_status.b          = c.b;
    s_status.w          = c.w;
    s_status.brightness = sk6812_get_brightness();
    s_status.led_count  = sk6812_count();
}

/* ------------------------ command handlers ----------------------------- */
static int handle_set_pattern(const uint8_t *p, uint16_t plen)
{
    if (plen < 1u) return -1;
    sk6812_color_t base = led_task_get_color();
    led_task_set_pattern((led_pattern_t)p[0], base);
    return 0;
}

static int handle_set_color(const uint8_t *p, uint16_t plen)
{
    if (plen < 4u) return -1;
    sk6812_color_t c = SK6812_RGBW(p[0], p[1], p[2], p[3]);
    led_task_set_color(c);
    return 0;
}

static int handle_set_brightness(const uint8_t *p, uint16_t plen)
{
    if (plen < 1u) return -1;
    led_task_set_brightness(p[0]);
    return 0;
}

static int handle_off(void)
{
    led_task_set_pattern(LED_PAT_OFF, SK6812_OFF);
    return 0;
}

static int handle_set_pixel(const uint8_t *p, uint16_t plen)
{
    if (plen < 6u) return -1;
    uint16_t idx = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    sk6812_color_t c = SK6812_RGBW(p[2], p[3], p[4], p[5]);
    return led_task_set_pixel(idx, c);
}

static int handle_set_pattern_color(const uint8_t *p, uint16_t plen)
{
    if (plen < 5u) return -1;
    sk6812_color_t c = SK6812_RGBW(p[1], p[2], p[3], p[4]);
    led_task_set_pattern((led_pattern_t)p[0], c);
    return 0;
}

static int handle_set_sleep_light(const uint8_t *p, uint16_t plen)
{
    if (plen < 2u) return -1;
    sleep_light_preset_t preset = (sleep_light_preset_t)p[0];
    uint8_t              pct    = p[1];
    if ((unsigned)preset >= (unsigned)SLEEP_LIGHT_PRESET_COUNT) return -2;
    led_task_set_sleep_light(preset, pct);
    return 0;
}

static int dispatch_cmd(const uint8_t *buf, uint16_t len)
{
    if (len < 1u) return -1;
    uint8_t op  = buf[0];
    const uint8_t *p = buf + 1;
    uint16_t       plen = (uint16_t)(len - 1u);

    switch (op) {
    case APP_LED_OP_SET_PATTERN:       return handle_set_pattern(p, plen);
    case APP_LED_OP_SET_COLOR:         return handle_set_color(p, plen);
    case APP_LED_OP_SET_BRIGHTNESS:    return handle_set_brightness(p, plen);
    case APP_LED_OP_OFF:               return handle_off();
    case APP_LED_OP_SET_PIXEL:         return handle_set_pixel(p, plen);
    case APP_LED_OP_SET_PATTERN_COLOR: return handle_set_pattern_color(p, plen);
    case APP_LED_OP_SET_SLEEP_LIGHT:   return handle_set_sleep_light(p, plen);
    default:                           return -2;
    }
}

/* ------------------------ GATT callbacks ------------------------------- */
static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len,
                         uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;

    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 1u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    int rc = dispatch_cmd((const uint8_t *)buf, len);
    if (rc != 0) {
        LOG_WRN("cmd rc=%d (op=0x%02x len=%u)",
                rc, ((const uint8_t *)buf)[0], (unsigned)len);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    /* Successful command -> hold off the state-machine LED policy and
     * push the new state out as a notify. */
    app_main_led_manual_override(0u);
    app_led_svc_publish_status();
    return (ssize_t)len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    refresh_cached_status();
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &s_status, sizeof(s_status));
}

static void sta_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr; (void)value;
}

BT_GATT_SERVICE_DEFINE(led_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_led_svc),
    BT_GATT_CHARACTERISTIC(&uuid_led_cmd.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, write_cmd, NULL),
    BT_GATT_CHARACTERISTIC(&uuid_led_sta.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_status, NULL, NULL),
    BT_GATT_CCC(sta_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ------------------------ public API ----------------------------------- */
int app_led_svc_init(void)
{
    (void)led_task_start();
    refresh_cached_status();
    LOG_INF("ready (UUID e700, leds=%u)", (unsigned)s_status.led_count);
    return 0;
}

void app_led_svc_publish_status(void)
{
    refresh_cached_status();
    (void)bt_gatt_notify(NULL, &led_svc.attrs[ATTR_IDX_STA_VAL],
                         &s_status, sizeof(s_status));
}
