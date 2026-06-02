/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "svc_ctrl.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_svc_ctrl, LOG_LEVEL_INF);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562U, (eXXX), 0x4000U, 0x8000U, 0x000000000001ULL)

#define BT_UUID_CTRL_SVC_VAL    UUID_BASE(0xe200)
#define BT_UUID_CTRL_CMD_VAL    UUID_BASE(0xe201)
#define BT_UUID_CTRL_RSP_VAL    UUID_BASE(0xe202)

static struct bt_uuid_128 uuid_ctrl_svc = BT_UUID_INIT_128(BT_UUID_CTRL_SVC_VAL);
static struct bt_uuid_128 uuid_ctrl_cmd = BT_UUID_INIT_128(BT_UUID_CTRL_CMD_VAL);
static struct bt_uuid_128 uuid_ctrl_rsp = BT_UUID_INIT_128(BT_UUID_CTRL_RSP_VAL);

static app_ctrl_evt_cb_t  s_cb;
static void              *s_cb_arg;
static uint8_t            s_resp_state;

/* Attribute layout for BT_GATT_SERVICE_DEFINE below:
 *   0: primary service
 *   1: char decl (cmd)
 *   2: char value (cmd)
 *   3: char decl (resp)
 *   4: char value (resp)        <-- ATTR_IDX_RSP_VAL
 *   5: CCC (resp)
 */
#define ATTR_IDX_RSP_VAL  4

static bool opcode_to_evt(uint8_t op, app_ctrl_evt_t *out)
{
    switch (op) {
    case APP_CTRL_OP_REBOOT:          *out = APP_CTRL_EVT_REBOOT;          return true;
    case APP_CTRL_OP_FACTORY_RESET:   *out = APP_CTRL_EVT_FACTORY_RESET;   return true;
    case APP_CTRL_OP_WIFI_DISCONNECT: *out = APP_CTRL_EVT_WIFI_DISCONNECT; return true;
    case APP_CTRL_OP_WIFI_RECONNECT:  *out = APP_CTRL_EVT_WIFI_RECONNECT;  return true;
    case APP_CTRL_OP_TCP_DISCONNECT:  *out = APP_CTRL_EVT_TCP_DISCONNECT;  return true;
    case APP_CTRL_OP_TCP_RECONNECT:   *out = APP_CTRL_EVT_TCP_RECONNECT;   return true;
    default:                                                                return false;
    }
}

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
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
    app_ctrl_evt_t evt;

    if (!opcode_to_evt(op, &evt)) {
        app_ctrl_respond(APP_CTRL_RSP_UNKNOWN_OP);
        return (ssize_t)len;
    }

    if (s_cb != NULL) {
        s_cb(evt, s_cb_arg);
    }
    /* OK is the default — app_main may overwrite via app_ctrl_respond(). */
    app_ctrl_respond(APP_CTRL_RSP_OK);
    return (ssize_t)len;
}

static ssize_t read_resp(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &s_resp_state, sizeof(s_resp_state));
}

static void resp_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr; (void)value;
}

BT_GATT_SERVICE_DEFINE(ctrl_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_ctrl_svc),
    BT_GATT_CHARACTERISTIC(&uuid_ctrl_cmd.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, write_cmd, NULL),
    BT_GATT_CHARACTERISTIC(&uuid_ctrl_rsp.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_resp, NULL, NULL),
    BT_GATT_CCC(resp_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int app_ctrl_init(app_ctrl_evt_cb_t cb, void *user_arg)
{
    s_cb         = cb;
    s_cb_arg     = user_arg;
    s_resp_state = APP_CTRL_RSP_OK;
    LOG_INF("ready (UUID e200)");
    return 0;
}

void app_ctrl_respond(uint8_t status)
{
    s_resp_state = status;
    (void)bt_gatt_notify(NULL, &ctrl_svc.attrs[ATTR_IDX_RSP_VAL],
                         &s_resp_state, sizeof(s_resp_state));
}
