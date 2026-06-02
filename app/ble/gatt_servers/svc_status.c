/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "svc_status.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_svc_status, LOG_LEVEL_INF);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562U, (eXXX), 0x4000U, 0x8000U, 0x000000000001ULL)

#define BT_UUID_STATUS_SVC_VAL     UUID_BASE(0xe100)
#define BT_UUID_STATUS_RPT_VAL     UUID_BASE(0xe101)

static struct bt_uuid_128 uuid_status_svc = BT_UUID_INIT_128(BT_UUID_STATUS_SVC_VAL);
static struct bt_uuid_128 uuid_status_rpt = BT_UUID_INIT_128(BT_UUID_STATUS_RPT_VAL);

static app_status_report_t s_last;

/* Index of the status-report VALUE attribute. With a single characteristic and
 * a CCC, attribute layout is:
 *   0: primary service
 *   1: char declaration
 *   2: char value           <-- ATTR_IDX_RPT_VAL
 *   3: CCC
 */
#define ATTR_IDX_RPT_VAL  2

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &s_last, sizeof(s_last));
}

static void status_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr; (void)value;
}

BT_GATT_SERVICE_DEFINE(status_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_status_svc),
    BT_GATT_CHARACTERISTIC(&uuid_status_rpt.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_status, NULL, NULL),
    BT_GATT_CCC(status_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

int app_status_init(void)
{
    memset(&s_last, 0, sizeof(s_last));
    LOG_INF("ready (UUID e100)");
    return 0;
}

void app_status_publish(const app_status_report_t *r)
{
    if (r == NULL) return;
    s_last = *r;
    (void)bt_gatt_notify(NULL, &status_svc.attrs[ATTR_IDX_RPT_VAL],
                         &s_last, sizeof(s_last));
}
