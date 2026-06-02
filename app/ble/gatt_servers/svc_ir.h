/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE IR remote control GATT service.
 *
 * Service UUID base : 5a637562-eXXX-4000-8000-000000000001
 *   e800 : service
 *   e801 : cmd   (write,  opcode byte + payload)
 *   e802 : state (read + notify, packed AC state report)
 *
 * Opcodes (phone -> device):
 *
 *   0x01 AIRCON_ACTION   : 1B brand + 1B action
 *                          brand   = ir_aircon_brand_t
 *                          action  = ir_aircon_action_t
 *                          -> POWER_ON / POWER_OFF / TEMP_UP / TEMP_DOWN
 *
 *   0x02 AIRCON_SET_TEMP : 1B brand + 1B temp (°C)
 *                          Samsung: requires preceding POWER_ON.
 *
 *   0x03 SET_CARRIER     : 2B little-endian carrier_hz (e.g. 38000)
 *
 *   0x10 SAMSUNG_RAW     : 1B fix_checksum_flag + N*7 frame bytes
 *                          N in {1,2,3}.  fix_checksum_flag != 0 rewrites
 *                          IRSamsungAc section checksums before TX.
 *
 *   0x20 RAW_SYMBOLS     : 1B carrier_kHz + N * (u16 mark_us, u16 space_us)
 *                          Bit-banged learn-and-replay channel; max N is
 *                          (MTU-2-1)/4 - fits ~50 pulses inside one MTU=247
 *                          notification. Caller is responsible for the
 *                          final gap symbol.
 *
 * State (e802) layout (4 bytes, packed LE):
 *   [0] aircon_powered  (1 = Samsung AC ON)
 *   [1] aircon_temp_c   (current Samsung set point; 0 if unknown)
 *   [2] carrier_hz      LSB
 *   [3] carrier_hz      MSB                 (divided by 1000 → kHz)
 */

#ifndef APP_BLE_SVC_IR_H
#define APP_BLE_SVC_IR_H

#ifdef __cplusplus
extern "C" {
#endif

#define APP_IR_OP_AIRCON_ACTION     0x01u
#define APP_IR_OP_AIRCON_SET_TEMP   0x02u
#define APP_IR_OP_SET_CARRIER       0x03u
#define APP_IR_OP_SAMSUNG_RAW       0x10u
#define APP_IR_OP_RAW_SYMBOLS       0x20u

/* Registers the e802 status notification. Must be called once after the
 * BLE controller is up (typically from bt_ready_cb()). */
int  app_ir_svc_init(void);

/* Refresh the cached IR state struct and push it out as a notification. */
void app_ir_svc_publish_status(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_SVC_IR_H */
