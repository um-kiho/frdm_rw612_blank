/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "svc_matter.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>

#include "matter_hub_core.h"

LOG_MODULE_REGISTER(app_svc_matter, LOG_LEVEL_INF);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562U, (eXXX), 0x4000U, 0x8000U, 0x000000000001ULL)

#define BT_UUID_MATTER_SVC_VAL  UUID_BASE(0xe900)
#define BT_UUID_MATTER_CMD_VAL  UUID_BASE(0xe901)
#define BT_UUID_MATTER_STA_VAL  UUID_BASE(0xe902)

static struct bt_uuid_128 uuid_matter_svc = BT_UUID_INIT_128(BT_UUID_MATTER_SVC_VAL);
static struct bt_uuid_128 uuid_matter_cmd = BT_UUID_INIT_128(BT_UUID_MATTER_CMD_VAL);
static struct bt_uuid_128 uuid_matter_sta = BT_UUID_INIT_128(BT_UUID_MATTER_STA_VAL);

_Static_assert(MATTER_HUB_STATUS_PACKED_LEN == 16u, "matter hub status packed len");

/*
 * ATTR layout (BT_GATT_SERVICE_DEFINE matter_svc):
 *   0 primary
 *   1 decl cmd    2 value cmd
 *   3 decl status 4 value status  <-- ATTR_IDX_STA_VAL
 *   5 CCC
 */
#define ATTR_IDX_STA_VAL  4

static uint8_t s_packed[MATTER_HUB_STATUS_PACKED_LEN];

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len,
                         uint16_t offset, uint8_t flags);
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset);
static void sta_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);

BT_GATT_SERVICE_DEFINE(matter_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_matter_svc),
    BT_GATT_CHARACTERISTIC(&uuid_matter_cmd.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, write_cmd, NULL),
    BT_GATT_CHARACTERISTIC(&uuid_matter_sta.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_status, NULL, NULL),
    BT_GATT_CCC(sta_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static void refresh_packed(void)
{
    matter_hub_pack_status(s_packed);
}

static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len,
                         uint16_t offset, uint8_t flags)
{
    (void)conn;
    (void)attr;
    (void)flags;

    if (offset != 0u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 1u) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    const uint8_t *p  = (const uint8_t *)buf;
    uint8_t       op   = p[0];
    int           rc;

    switch (op) {
    case APP_MATTER_OP_FACTORY_RESET:
        rc = matter_hub_factory_reset();
        break;

    case APP_MATTER_OP_WINDOW_OPEN: {
        uint32_t dur = 0u;
        if (len >= 3u) {
            dur = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
        }
        rc = matter_hub_commission_window_open(dur, matter_hub_cached_uptime_s());
        break;
    }

    case APP_MATTER_OP_WINDOW_CLOSE:
        rc = matter_hub_commission_window_close();
        break;

    case APP_MATTER_OP_DEV_SIM_FABRIC:
        rc = matter_hub_dev_simulate_fabric_commissioned();
        break;

    default:
        rc = -2;
        break;
    }

    if (rc != 0) {
        LOG_WRN("cmd rc=%d op=0x%02x len=%u", rc, op, (unsigned)len);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    refresh_packed();
    (void)bt_gatt_notify(NULL, &matter_svc.attrs[ATTR_IDX_STA_VAL],
                         s_packed, sizeof(s_packed));
    return (ssize_t)len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    refresh_packed();
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             s_packed, sizeof(s_packed));
}

static void sta_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    (void)attr;
    if (value == BT_GATT_CCC_NOTIFY) {
        refresh_packed();
        (void)bt_gatt_notify(NULL, &matter_svc.attrs[ATTR_IDX_STA_VAL],
                             s_packed, sizeof(s_packed));
    }
}

int app_matter_svc_init(void)
{
    refresh_packed();
    LOG_INF("ready (UUID e900)");
    return 0;
}

void app_matter_svc_publish_status(void)
{
    refresh_packed();
    (void)bt_gatt_notify(NULL, &matter_svc.attrs[ATTR_IDX_STA_VAL],
                         s_packed, sizeof(s_packed));
}
