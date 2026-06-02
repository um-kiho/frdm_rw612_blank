/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE status GATT service (server #2).
 *
 * Exposes a single notify+read characteristic carrying a small fixed-size
 * status record. app_main publishes a new record whenever a relevant state
 * changes (Wi-Fi up/down, TCP up/down, sensor rx count).
 *
 * Service UUID base : 5a637562-eXXX-4000-8000-000000000001
 *   e100 : service
 *   e101 : status report (read + notify, sizeof(app_status_report_t))
 */

#ifndef APP_BLE_SVC_STATUS_H
#define APP_BLE_SVC_STATUS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compact LE-encoded report. Total size: 32 bytes. Requires MTU>=35 to fit
 * a single ATT notification; EdgeFast negotiates a larger MTU when
 * CONFIG_BT_DATA_LEN_UPDATE=y (already on in prj.conf). */
typedef struct __attribute__((packed)) app_status_report {
    uint8_t  wifi_state;        /* 0=disc, 1=connecting, 2=connected      */
    uint8_t  tcp_state;         /* 0=disc, 1=connecting, 2=connected      */
    uint8_t  prov_state;        /* mirrors svc_prov state byte            */
    uint8_t  sensor_state;      /* BLE central: 0=idle, 1=scanning, 2=conn*/
    uint32_t uptime_s;          /* monotonic seconds since boot           */
    uint16_t sensor_rx_count;   /* BLE central: total notifications rx'd  */
    uint8_t  lux1_err;          /* 0=ok, otherwise BH1750_ERR_*           */
    uint8_t  lux2_err;
    uint32_t lux1_x100;         /* BH1750 0x23 channel, 0.01 lx units     */
    uint32_t lux2_x100;         /* BH1750 0x5C channel, 0.01 lx units     */
    int8_t   amg_err;           /* 0=ok, otherwise AMG8833_ERR_*          */
    uint8_t  _amg_pad;
    int16_t  amg_min_q2;        /* AMG8833 pixel min, 0.25 C units        */
    int16_t  amg_avg_q2;        /* AMG8833 pixel avg                      */
    int16_t  amg_max_q2;        /* AMG8833 pixel max                      */
    uint32_t radar_rx_bytes;    /* radar UART byte counter (format: TBD)  */
} app_status_report_t;

int  app_status_init   (void);
void app_status_publish(const app_status_report_t *r);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_SVC_STATUS_H */
