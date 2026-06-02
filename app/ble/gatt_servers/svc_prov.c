/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE provisioning GATT service implementation (EdgeFast / Zephyr-style API).
 *
 * Service UUID base : 5a637562-eXXX-4000-8000-000000000001
 *                     ^z^c^u^b^   ^         ^
 *                     "zcub"      where eXXX encodes the per-characteristic id.
 *
 *   e000 : service
 *   e001 : SSID        (write, up to 32 bytes)
 *   e002 : PASSWORD    (write, up to 64 bytes)
 *   e003 : SECURITY    (write, up to 16 bytes, ascii: OPEN / WPA2 / WPA2_WPA3 / WPA3_SAE)
 *   e004 : HOST_IP     (write, up to 64 bytes, IPv4 dotted or short FQDN)
 *   e005 : PORT        (write, exactly 2 bytes, little-endian uint16)
 *   e006 : COMMIT      (write, first byte = APP_PROV_OP_* opcode)
 *   e007 : STATE       (read + notify, 1 byte = APP_PROV_STATE_*)
 */

#include "svc_prov.h"
#include "app_nvram.h"

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_svc_prov, LOG_LEVEL_INF);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562U, (eXXX), 0x4000U, 0x8000U, 0x000000000001ULL)

#define BT_UUID_PROV_SVC_VAL     UUID_BASE(0xe000)
#define BT_UUID_PROV_SSID_VAL    UUID_BASE(0xe001)
#define BT_UUID_PROV_PASS_VAL    UUID_BASE(0xe002)
#define BT_UUID_PROV_SEC_VAL     UUID_BASE(0xe003)
#define BT_UUID_PROV_HOST_VAL    UUID_BASE(0xe004)
#define BT_UUID_PROV_PORT_VAL    UUID_BASE(0xe005)
#define BT_UUID_PROV_COMMIT_VAL  UUID_BASE(0xe006)
#define BT_UUID_PROV_STATE_VAL   UUID_BASE(0xe007)

static struct bt_uuid_128 uuid_prov_svc    = BT_UUID_INIT_128(BT_UUID_PROV_SVC_VAL);
static struct bt_uuid_128 uuid_prov_ssid   = BT_UUID_INIT_128(BT_UUID_PROV_SSID_VAL);
static struct bt_uuid_128 uuid_prov_pass   = BT_UUID_INIT_128(BT_UUID_PROV_PASS_VAL);
static struct bt_uuid_128 uuid_prov_sec    = BT_UUID_INIT_128(BT_UUID_PROV_SEC_VAL);
static struct bt_uuid_128 uuid_prov_host   = BT_UUID_INIT_128(BT_UUID_PROV_HOST_VAL);
static struct bt_uuid_128 uuid_prov_port   = BT_UUID_INIT_128(BT_UUID_PROV_PORT_VAL);
static struct bt_uuid_128 uuid_prov_commit = BT_UUID_INIT_128(BT_UUID_PROV_COMMIT_VAL);
static struct bt_uuid_128 uuid_prov_state  = BT_UUID_INIT_128(BT_UUID_PROV_STATE_VAL);

static app_nvram_data_t   s_staging;
static uint8_t            s_state;
static app_prov_evt_cb_t  s_cb;
static void              *s_cb_arg;

/* Index of the "state" characteristic VALUE attribute inside prov_svc.attrs[].
 * BT_GATT_SERVICE_DEFINE expands each BT_GATT_CHARACTERISTIC into two attrs
 * (declaration + value). With 7 characteristics declared in order, the value
 * attributes are at indices 2, 4, 6, 8, 10, 12, 14.  The state CCC follows at
 * index 15. Keep this in sync with the BT_GATT_SERVICE_DEFINE block below. */
#define ATTR_IDX_STATE_VAL   14

static ssize_t write_string_field(char *dst, uint32_t maxlen,
                                  const void *buf, uint16_t len, uint16_t offset)
{
    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len > maxlen) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    memset(dst, 0, maxlen + 1u);
    memcpy(dst, buf, len);
    return (ssize_t)len;
}

static ssize_t write_ssid(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;
    return write_string_field(s_staging.ssid, APP_NVRAM_SSID_MAXLEN, buf, len, offset);
}

static ssize_t write_pass(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;
    return write_string_field(s_staging.password, APP_NVRAM_PASS_MAXLEN, buf, len, offset);
}

static ssize_t write_sec(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;
    return write_string_field(s_staging.security, APP_NVRAM_SEC_MAXLEN, buf, len, offset);
}

static ssize_t write_host(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;
    return write_string_field(s_staging.host_ip, APP_NVRAM_HOST_MAXLEN, buf, len, offset);
}

static ssize_t write_port(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;
    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len != sizeof(uint16_t)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint16_t p;
    memcpy(&p, buf, sizeof(p));     /* little-endian wire format */
    s_staging.port = p;
    return (ssize_t)len;
}

static void emit(app_prov_evt_t evt, uint8_t state)
{
    s_state = state;
    if (s_cb != NULL) {
        s_cb(evt, s_cb_arg);
    }
}

static ssize_t write_commit(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;
    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 1u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    uint8_t op = ((const uint8_t *)buf)[0];

    if (op == APP_PROV_OP_RESET) {
        (void)app_nvram_reset();
        memset(&s_staging, 0, sizeof(s_staging));
        emit(APP_PROV_EVT_RESET, APP_PROV_STATE_IDLE);
        app_prov_set_state(APP_PROV_STATE_IDLE);
        return (ssize_t)len;
    }

    if (op != APP_PROV_OP_COMMIT) {
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

    if (!app_nvram_is_valid(&s_staging)) {
        emit(APP_PROV_EVT_COMMIT_INVALID, APP_PROV_STATE_INVALID);
        app_prov_set_state(APP_PROV_STATE_INVALID);
        return (ssize_t)len;
    }

    if (app_nvram_save(&s_staging) != APP_NVRAM_OK) {
        emit(APP_PROV_EVT_COMMIT_IO_ERROR, APP_PROV_STATE_IO_ERROR);
        app_prov_set_state(APP_PROV_STATE_IO_ERROR);
        return (ssize_t)len;
    }

    emit(APP_PROV_EVT_COMMIT_OK, APP_PROV_STATE_COMMITTED);
    app_prov_set_state(APP_PROV_STATE_COMMITTED);
    return (ssize_t)len;
}

static ssize_t read_state(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &s_state, sizeof(s_state));
}

static void state_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr; (void)value;
}

BT_GATT_SERVICE_DEFINE(prov_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_prov_svc),

    BT_GATT_CHARACTERISTIC(&uuid_prov_ssid.uuid,   BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,     NULL, write_ssid,   NULL),
    BT_GATT_CHARACTERISTIC(&uuid_prov_pass.uuid,   BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,     NULL, write_pass,   NULL),
    BT_GATT_CHARACTERISTIC(&uuid_prov_sec.uuid,    BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,     NULL, write_sec,    NULL),
    BT_GATT_CHARACTERISTIC(&uuid_prov_host.uuid,   BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,     NULL, write_host,   NULL),
    BT_GATT_CHARACTERISTIC(&uuid_prov_port.uuid,   BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,     NULL, write_port,   NULL),
    BT_GATT_CHARACTERISTIC(&uuid_prov_commit.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,     NULL, write_commit, NULL),
    BT_GATT_CHARACTERISTIC(&uuid_prov_state.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,      read_state, NULL,   NULL),
    BT_GATT_CCC(state_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int app_prov_init(app_prov_evt_cb_t cb, void *user_arg)
{
    s_cb     = cb;
    s_cb_arg = user_arg;
    s_state  = APP_PROV_STATE_IDLE;

    /* Pre-populate staging with existing NVRAM so a partial write
     * (e.g. password-only) keeps the other fields intact. */
    memset(&s_staging, 0, sizeof(s_staging));
    (void)app_nvram_load(&s_staging);

    LOG_INF("ready (UUID e000)");
    return 0;
}

void app_prov_set_state(uint8_t state)
{
    s_state = state;
    /* NULL conn => notify all subscribers on the state CHRC value attr. */
    (void)bt_gatt_notify(NULL, &prov_svc.attrs[ATTR_IDX_STATE_VAL],
                         &s_state, sizeof(s_state));
}
