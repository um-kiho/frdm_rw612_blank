/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE Central implementation. The flow is:
 *   1. bt_le_scan_start(passive) -> scan_cb
 *   2. In scan_cb, parse adv data; if BT_DATA_UUID128_* contains the sensor
 *      service UUID, stop scanning and bt_conn_le_create().
 *   3. On "connected", run bt_gatt_discover for:
 *         a. primary service (e300)
 *         b. data characteristic (e301)
 *         c. CCC descriptor
 *   4. bt_gatt_subscribe() to enable notifications.
 *   5. notify_func() pushes payload to the application via rx_cb.
 *
 * The discover_params.uuid pointer must remain valid for the lifetime of the
 * discover call, so the per-stage UUIDs are kept in module-static storage.
 */

#include "ble_central.h"

#include <string.h>
#include <stdint.h>
#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ble_central, LOG_LEVEL_INF);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562, (eXXX), 0x4000, 0x8000, 0x000000000001)

#define SENSOR_SVC_VAL    UUID_BASE(0xe300)
#define SENSOR_DATA_VAL   UUID_BASE(0xe301)

static struct bt_uuid_128 s_sensor_svc  = BT_UUID_INIT_128(SENSOR_SVC_VAL);
static struct bt_uuid_128 s_sensor_data = BT_UUID_INIT_128(SENSOR_DATA_VAL);
static struct bt_uuid_16  s_ccc_uuid    = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

/* Module state. Only one sensor connection is supported at a time. */
static struct bt_conn                  *s_conn;
static struct bt_gatt_discover_params   s_disc;
static struct bt_gatt_subscribe_params  s_sub;
static struct bt_uuid_128               s_disc_uuid;   /* swappable storage   */

static app_central_evt_cb_t s_evt_cb;
static app_central_rx_cb_t  s_rx_cb;
static void                *s_user_arg;
static volatile bool        s_running;
static volatile bool        s_subscribed;

static void emit(app_central_evt_t e)
{
    if (s_evt_cb != NULL) {
        s_evt_cb(e, s_user_arg);
    }
}

static int start_scan(void);

static uint8_t notify_func(struct bt_conn *conn,
                           struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length)
{
    (void)conn; (void)params;
    if (data == NULL) {
        /* Subscription has been removed. */
        s_subscribed = false;
        return BT_GATT_ITER_STOP;
    }
    if (s_rx_cb != NULL) {
        s_rx_cb((const uint8_t *)data, (size_t)length, s_user_arg);
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    if (attr == NULL) {
        LOG_INF("[central] discover done (no match)");
        memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    if (!bt_uuid_cmp(s_disc.uuid, &s_sensor_svc.uuid)) {
        /* Primary service located -> look for the data characteristic. */
        memcpy(&s_disc_uuid, &s_sensor_data, sizeof(s_sensor_data));
        s_disc.uuid         = &s_disc_uuid.uuid;
        s_disc.start_handle = attr->handle + 1;
        s_disc.type         = BT_GATT_DISCOVER_CHARACTERISTIC;
        (void)bt_gatt_discover(conn, &s_disc);
        return BT_GATT_ITER_STOP;
    }

    if (!bt_uuid_cmp(s_disc.uuid, &s_sensor_data.uuid)) {
        /* Characteristic located -> look for the CCC descriptor. */
        s_sub.value_handle  = bt_gatt_attr_value_handle(attr);
        s_disc.uuid         = &s_ccc_uuid.uuid;
        s_disc.start_handle = attr->handle + 2;
        s_disc.type         = BT_GATT_DISCOVER_DESCRIPTOR;
        (void)bt_gatt_discover(conn, &s_disc);
        return BT_GATT_ITER_STOP;
    }

    /* CCC located -> subscribe. */
    s_sub.notify     = notify_func;
    s_sub.value      = BT_GATT_CCC_NOTIFY;
    s_sub.ccc_handle = attr->handle;

    int err = bt_gatt_subscribe(conn, &s_sub);
    if (err && err != -EALREADY) {
        LOG_ERR("[central] bt_gatt_subscribe err=%d", err);
        emit(APP_CENTRAL_EVT_ERROR);
    } else {
        s_subscribed = true;
        LOG_INF("[central] subscribed to sensor notifications");
        emit(APP_CENTRAL_EVT_SUBSCRIBED);
    }
    return BT_GATT_ITER_STOP;
}

static int begin_discovery(struct bt_conn *conn)
{
    memset(&s_disc, 0, sizeof(s_disc));
    memcpy(&s_disc_uuid, &s_sensor_svc, sizeof(s_sensor_svc));
    s_disc.uuid         = &s_disc_uuid.uuid;
    s_disc.func         = discover_func;
    s_disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    s_disc.end_handle   = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    s_disc.type         = BT_GATT_DISCOVER_PRIMARY;
    return bt_gatt_discover(conn, &s_disc);
}

static bool eir_found(struct bt_data *data, void *user_data)
{
    bt_addr_le_t *addr = (bt_addr_le_t *)user_data;

    if (data->type != BT_DATA_UUID128_ALL &&
        data->type != BT_DATA_UUID128_SOME) {
        return true; /* keep parsing */
    }
    if ((data->data_len % 16u) != 0u) {
        return true;
    }

    for (uint16_t i = 0; i < data->data_len; i += 16u) {
        struct bt_uuid_128 u = {.uuid = {.type = BT_UUID_TYPE_128}};
        memcpy(u.val, &data->data[i], 16);
        if (bt_uuid_cmp(&u.uuid, &s_sensor_svc.uuid) != 0) {
            continue;
        }

        /* Match. Stop scanning and attempt a connection. */
        (void)bt_le_scan_stop();
        int err = bt_conn_le_create(addr,
                                    BT_CONN_LE_CREATE_CONN,
                                    BT_LE_CONN_PARAM_DEFAULT,
                                    &s_conn);
        if (err) {
            LOG_ERR("[central] bt_conn_le_create err=%d, resume scan", err);
            (void)start_scan();
        }
        return false; /* stop parsing */
    }
    return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                    struct net_buf_simple *buf)
{
    (void)rssi;
    if (adv_type != BT_GAP_ADV_TYPE_ADV_IND &&
        adv_type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
        return;
    }
    bt_data_parse(buf, eir_found, (void *)addr);
}

static int start_scan(void)
{
    struct bt_le_scan_param p = {
        .type     = BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window   = BT_GAP_SCAN_FAST_WINDOW,
    };
    int err = bt_le_scan_start(&p, scan_cb);
    if (err) {
        LOG_ERR("[central] bt_le_scan_start err=%d", err);
        emit(APP_CENTRAL_EVT_ERROR);
        return err;
    }
    LOG_INF("[central] scanning for sensor (UUID e300)");
    emit(APP_CENTRAL_EVT_SCANNING);
    return 0;
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    /* Only react to our sensor link, not phone links handled by svc_prov. */
    if (conn != s_conn) {
        return;
    }
    if (err) {
        LOG_ERR("[central] sensor connect failed err=%u", err);
        bt_conn_unref(s_conn);
        s_conn = NULL;
        emit(APP_CENTRAL_EVT_ERROR);
        if (s_running) (void)start_scan();
        return;
    }
    LOG_INF("[central] sensor connected");
    emit(APP_CENTRAL_EVT_CONNECTED);
    (void)begin_discovery(conn);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    if (conn != s_conn) {
        return;
    }
    LOG_INF("[central] sensor disconnected (reason=%u)", reason);
    bt_conn_unref(s_conn);
    s_conn       = NULL;
    s_subscribed = false;
    emit(APP_CENTRAL_EVT_DISCONNECTED);
    if (s_running) {
        (void)start_scan();
    }
}

static struct bt_conn_cb s_conn_cb = {
    .connected    = connected_cb,
    .disconnected = disconnected_cb,
};

int app_central_start(app_central_evt_cb_t evt_cb,
                      app_central_rx_cb_t  rx_cb,
                      void *user_arg)
{
    if (s_running) return 0;

    s_evt_cb   = evt_cb;
    s_rx_cb    = rx_cb;
    s_user_arg = user_arg;
    s_running  = true;

    bt_conn_cb_register(&s_conn_cb);
    return start_scan();
}

void app_central_stop(void)
{
    s_running = false;
    (void)bt_le_scan_stop();
    if (s_conn != NULL) {
        (void)bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

bool app_central_is_connected(void)
{
    return (s_conn != NULL) && s_subscribed;
}
