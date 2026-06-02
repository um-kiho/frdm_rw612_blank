/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "svc_ota.h"
#include "app_ota.h"
#include "app_ota_http.h"

#include <string.h>
#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(svc_ota, LOG_LEVEL_INF);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562, (eXXX), 0x4000, 0x8000, 0x000000000001)

#define BT_UUID_OTA_SVC_VAL     UUID_BASE(0xe600)
#define BT_UUID_OTA_CTRL_VAL    UUID_BASE(0xe601)
#define BT_UUID_OTA_STATUS_VAL  UUID_BASE(0xe602)

static struct bt_uuid_128 uuid_ota_svc    = BT_UUID_INIT_128(BT_UUID_OTA_SVC_VAL);
static struct bt_uuid_128 uuid_ota_ctrl   = BT_UUID_INIT_128(BT_UUID_OTA_CTRL_VAL);
static struct bt_uuid_128 uuid_ota_status = BT_UUID_INIT_128(BT_UUID_OTA_STATUS_VAL);

#pragma pack(push, 1)
typedef struct ota_status_pdu {
    uint8_t  state;
    uint8_t  last_err;
    uint32_t bytes_received;
    uint32_t total_size;
} ota_status_pdu_t;
#pragma pack(pop)

static ota_status_pdu_t s_last_pdu;
static uint16_t         s_subscribers;

/* ------------------------------------------------------------------------- *
 * little-endian helpers
 * ------------------------------------------------------------------------- */
static inline uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t rd_u32le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------------- *
 * STATUS read + notify
 * ------------------------------------------------------------------------- */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &s_last_pdu, sizeof(s_last_pdu));
}

static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr;
    if (value == BT_GATT_CCC_NOTIFY) {
        s_subscribers++;
    } else if (s_subscribers > 0u) {
        s_subscribers--;
    }
}

/* ------------------------------------------------------------------------- *
 * CTRL write
 * ------------------------------------------------------------------------- */
static int dispatch_ctrl(const uint8_t *p, uint16_t plen)
{
    if (plen < 1u) return APP_OTA_ERR_PARAM;
    uint8_t op = p[0];
    const uint8_t *arg = p + 1;
    uint16_t alen = plen - 1u;

    switch (op) {
    case APP_OTA_OP_BEGIN: {
        if (alen < 4u) return APP_OTA_ERR_PARAM;
        return app_ota_begin(rd_u32le(arg));
    }
    case APP_OTA_OP_CHUNK: {
        if (alen < 6u) return APP_OTA_ERR_PARAM;
        uint32_t off  = rd_u32le(arg);
        uint16_t dlen = rd_u16le(arg + 4);
        if (alen < 6u + dlen) return APP_OTA_ERR_PARAM;
        return app_ota_chunk(off, arg + 6, dlen);
    }
    case APP_OTA_OP_COMMIT: {
        int rc = app_ota_commit(alen ? arg : NULL, alen);
        if (rc == APP_OTA_OK) {
            /* Leave the client a window to read the COMMITTED status. */
            app_ota_reboot_after_ms(800u);
        }
        return rc;
    }
    case APP_OTA_OP_ABORT:
        app_ota_abort();
        app_ota_http_stop();
        return APP_OTA_OK;
    case APP_OTA_OP_REBOOT:
        app_ota_reboot_after_ms(200u);
        return APP_OTA_OK;
    case APP_OTA_OP_HTTP_PULL: {
        /* [flags:u8][iurl_len:u16][image_url][hurl_len:u16][header_url] */
        if (alen < 1u + 2u) return APP_OTA_ERR_PARAM;
        uint8_t  flags    = arg[0];
        uint16_t iurl_len = rd_u16le(arg + 1);
        if (iurl_len == 0u || iurl_len > APP_OTA_HTTP_URL_MAX) {
            return APP_OTA_ERR_PARAM;
        }
        size_t need = 1u + 2u + iurl_len + 2u;
        if (alen < need) return APP_OTA_ERR_PARAM;
        uint16_t hurl_len = rd_u16le(arg + 1 + 2 + iurl_len);
        if (hurl_len > APP_OTA_HTTP_URL_MAX) return APP_OTA_ERR_PARAM;
        need += hurl_len;
        if (alen < need) return APP_OTA_ERR_PARAM;

        static char image_url [APP_OTA_HTTP_URL_MAX + 1];
        static char header_url[APP_OTA_HTTP_URL_MAX + 1];
        memcpy(image_url, arg + 1 + 2, iurl_len);
        image_url[iurl_len] = '\0';
        if (hurl_len > 0u) {
            memcpy(header_url, arg + 1 + 2 + iurl_len + 2, hurl_len);
            header_url[hurl_len] = '\0';
        } else {
            header_url[0] = '\0';
        }

        app_ota_http_opts_t opts = {
            .image_url   = image_url,
            .header_url  = (hurl_len > 0u) ? header_url : NULL,
            .auto_commit = (flags & APP_OTA_HTTP_FLAG_AUTO_COMMIT) != 0u,
        };
        int rc = app_ota_http_start(&opts);
        return (rc == 0) ? APP_OTA_OK : APP_OTA_ERR_WRITE_FAILED;
    }
    default:
        return APP_OTA_ERR_PARAM;
    }
}

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len,
                          uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;
    if (offset != 0u) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (len == 0u)    return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);

    int rc = dispatch_ctrl((const uint8_t *)buf, len);
    if (rc != APP_OTA_OK) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return len;
}

/* ------------------------------------------------------------------------- *
 * GATT layout
 *   0: primary service
 *   1: char decl (CTRL)
 *   2: char value (CTRL)
 *   3: char decl (STATUS)
 *   4: char value (STATUS)   <-- ATTR_IDX_STATUS_VAL
 *   5: CCC (STATUS)
 * ------------------------------------------------------------------------- */
#define ATTR_IDX_STATUS_VAL  4

BT_GATT_SERVICE_DEFINE(ota_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_ota_svc),

    BT_GATT_CHARACTERISTIC(&uuid_ota_ctrl.uuid,
                           BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl, NULL),

    BT_GATT_CHARACTERISTIC(&uuid_ota_status.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_status, NULL, NULL),
    BT_GATT_CCC(status_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ------------------------------------------------------------------------- *
 * app_ota_status_cb hook
 * ------------------------------------------------------------------------- */
static void ota_status_cb(const app_ota_status_t *s, void *arg)
{
    (void)arg;
    if (s == NULL) return;

    s_last_pdu.state          = s->state;
    s_last_pdu.last_err       = s->last_err;
    s_last_pdu.bytes_received = s->bytes_received;
    s_last_pdu.total_size     = s->total_size;

    if (s_subscribers > 0u) {
        (void)bt_gatt_notify(NULL, &ota_svc.attrs[ATTR_IDX_STATUS_VAL],
                             &s_last_pdu, sizeof(s_last_pdu));
    }
}

int app_ota_svc_init(void)
{
    memset(&s_last_pdu, 0, sizeof(s_last_pdu));
    int rc = app_ota_init(ota_status_cb, NULL);
    if (rc != APP_OTA_OK) {
        LOG_ERR("[svc_ota] app_ota_init rc=%d", rc);
        return rc;
    }
    LOG_INF("[svc_ota] ready (UUID e600, ctrl e601, status e602)");
    return 0;
}
