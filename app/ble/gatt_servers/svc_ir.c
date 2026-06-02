/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "svc_ir.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/logging/log.h>

#include "ir_tx.h"
#include "ir_aircon.h"

LOG_MODULE_REGISTER(app_svc_ir, LOG_LEVEL_INF);

#define UUID_BASE(eXXX) \
    BT_UUID_128_ENCODE(0x5a637562U, (eXXX), 0x4000U, 0x8000U, 0x000000000001ULL)

#define BT_UUID_IR_SVC_VAL      UUID_BASE(0xe800)
#define BT_UUID_IR_CMD_VAL      UUID_BASE(0xe801)
#define BT_UUID_IR_STA_VAL      UUID_BASE(0xe802)

static struct bt_uuid_128 uuid_ir_svc = BT_UUID_INIT_128(BT_UUID_IR_SVC_VAL);
static struct bt_uuid_128 uuid_ir_cmd = BT_UUID_INIT_128(BT_UUID_IR_CMD_VAL);
static struct bt_uuid_128 uuid_ir_sta = BT_UUID_INIT_128(BT_UUID_IR_STA_VAL);

/* Wire layout per svc_ir.h - keep this packed so notify() copies are
 * single memcpy from struct to ATT payload. */
typedef struct __attribute__((packed)) {
    uint8_t  aircon_powered;
    uint8_t  aircon_temp_c;
    uint16_t carrier_hz;
} ir_status_t;

static ir_status_t s_status;

/* Attribute layout in BT_GATT_SERVICE_DEFINE:
 *   0: primary service
 *   1: char decl (cmd)
 *   2: char value (cmd)
 *   3: char decl (state)
 *   4: char value (state)   <-- ATTR_IDX_STA_VAL
 *   5: CCC (state)
 */
#define ATTR_IDX_STA_VAL  4

/* Max ir_symbol_t entries that fit in a single OP_RAW_SYMBOLS payload.
 *  - 1 opcode byte + 1 carrier_kHz byte + N * 4 (u16 mark + u16 space)
 *  - With MTU=247 the ATT_MTU - 3 = 244 -> N <= (244 - 2) / 4 = 60. */
#define IR_RAW_MAX_PER_WRITE   60u

static void refresh_cached_status(void)
{
    s_status.aircon_powered = ir_aircon_is_powered(IR_AIRCON_BRAND_SAMSUNG) ? 1u : 0u;
    s_status.aircon_temp_c  = ir_aircon_get_temp_c(IR_AIRCON_BRAND_SAMSUNG);
    s_status.carrier_hz     = (uint16_t)ir_tx_get_carrier_hz();
}

/* ------------------------ command handlers ----------------------------- */
static int handle_aircon_action(const uint8_t *p, uint16_t plen)
{
    if (plen < 2u) return -1;
    return ir_aircon_send((unsigned)p[0], (unsigned)p[1]);
}

static int handle_aircon_set_temp(const uint8_t *p, uint16_t plen)
{
    if (plen < 2u) return -1;
    return ir_aircon_set_temp_c((unsigned)p[0], p[1]);
}

static int handle_set_carrier(const uint8_t *p, uint16_t plen)
{
    if (plen < 2u) return -1;
    uint32_t hz = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    return ir_tx_set_carrier_hz(hz);
}

static int handle_samsung_raw(const uint8_t *p, uint16_t plen)
{
    if (plen < 1u + 7u) return -1;
    uint8_t fix_cs = p[0];
    const uint8_t *frame = p + 1;
    uint16_t flen = (uint16_t)(plen - 1u);
    if ((flen % 7u) != 0u || flen > 21u) return -2;
    return ir_aircon_samsung_send_raw(frame, flen, fix_cs != 0u);
}

static int handle_raw_symbols(const uint8_t *p, uint16_t plen)
{
    if (plen < 1u + 4u) return -1;
    uint32_t carrier_hz = (uint32_t)p[0] * 1000u;
    const uint8_t *sym_bytes = p + 1u;
    uint16_t sym_len = (uint16_t)(plen - 1u);
    if ((sym_len % 4u) != 0u) return -2;
    size_t count = sym_len / 4u;
    if (count == 0u || count > IR_RAW_MAX_PER_WRITE) return -3;

    static ir_symbol_t syms[IR_RAW_MAX_PER_WRITE];
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *e = sym_bytes + i * 4u;
        syms[i].mark_us  = (uint16_t)(e[0] | ((uint16_t)e[1] << 8));
        syms[i].space_us = (uint16_t)(e[2] | ((uint16_t)e[3] << 8));
    }
    if (carrier_hz) ir_tx_set_carrier_hz(carrier_hz);
    return ir_tx_send_symbols(syms, count);
}

static int dispatch_cmd(const uint8_t *buf, uint16_t len)
{
    if (len < 1u) return -1;
    uint8_t op   = buf[0];
    const uint8_t *p = buf + 1;
    uint16_t plen   = (uint16_t)(len - 1u);

    switch (op) {
    case APP_IR_OP_AIRCON_ACTION:    return handle_aircon_action(p, plen);
    case APP_IR_OP_AIRCON_SET_TEMP:  return handle_aircon_set_temp(p, plen);
    case APP_IR_OP_SET_CARRIER:      return handle_set_carrier(p, plen);
    case APP_IR_OP_SAMSUNG_RAW:      return handle_samsung_raw(p, plen);
    case APP_IR_OP_RAW_SYMBOLS:      return handle_raw_symbols(p, plen);
    default:                         return -2;
    }
}

/* ------------------------ GATT callbacks ------------------------------- */
static ssize_t write_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         const void *buf, uint16_t len,
                         uint16_t offset, uint8_t flags)
{
    (void)conn; (void)attr; (void)flags;

    if (offset != 0u) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    if (len < 1u)     return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);

    int rc = dispatch_cmd((const uint8_t *)buf, len);
    if (rc != 0) {
        LOG_WRN("cmd rc=%d (op=0x%02x len=%u)",
                rc, ((const uint8_t *)buf)[0], (unsigned)len);
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    app_ir_svc_publish_status();
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

BT_GATT_SERVICE_DEFINE(ir_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_ir_svc),
    BT_GATT_CHARACTERISTIC(&uuid_ir_cmd.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, write_cmd, NULL),
    BT_GATT_CHARACTERISTIC(&uuid_ir_sta.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ, read_status, NULL, NULL),
    BT_GATT_CCC(sta_ccc_cfg_changed,
                BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ------------------------ public API ----------------------------------- */
int app_ir_svc_init(void)
{
    int rc = ir_aircon_init();

    if (rc != 0) {
        LOG_WRN("IR init rc=%d", rc);
    }

    refresh_cached_status();
    LOG_INF("ready (UUID e800, carrier=%u Hz)",
            (unsigned)s_status.carrier_hz);
    return 0;
}

void app_ir_svc_publish_status(void)
{
    refresh_cached_status();
    (void)bt_gatt_notify(NULL, &ir_svc.attrs[ATTR_IDX_STA_VAL],
                         &s_status, sizeof(s_status));
}
