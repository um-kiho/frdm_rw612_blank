/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * OTA front-end.
 *
 * Uses Zephyr's flash image and MCUboot APIs to expose a small,
 * transport-agnostic state machine:
 *
 *     IDLE --begin(N)--> READY --chunk(0..N-1)--> WRITING --commit(hdr)--> COMMITTED
 *       ^                  |                        |                          |
 *       +-- abort() <------+ <----------------------+---------------- reboot --+
 *                                                  |
 *                                                  +--> FAILED
 *
 * Transports that can drive this state machine:
 *   - BLE (svc_ota, UUID e600) - implemented in this tree
 *   - TCP / HTTP / serial      - future, just call the same APIs
 *
 * The secondary image slot is the MCUboot upload slot.
 * A successful app_ota_commit() requests an upgrade and reboots the board.
 */

#ifndef APP_OTA_H
#define APP_OTA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum app_ota_state {
    APP_OTA_STATE_IDLE       = 0,
    APP_OTA_STATE_READY      = 1,   /* begin(size) accepted, awaiting chunks */
    APP_OTA_STATE_WRITING    = 2,   /* at least one chunk programmed         */
    APP_OTA_STATE_COMMITTED  = 3,   /* commit accepted, reboot pending       */
    APP_OTA_STATE_FAILED     = 4,
} app_ota_state_t;

typedef enum app_ota_err {
    APP_OTA_OK                 = 0,
    APP_OTA_ERR_PARAM          = 1,
    APP_OTA_ERR_INVALID_STATE  = 2,
    APP_OTA_ERR_INVALID_OFFSET = 3,
    APP_OTA_ERR_WRITE_FAILED   = 4,
    APP_OTA_ERR_COMMIT_FAILED  = 5,
    APP_OTA_ERR_ABORTED        = 6,
    APP_OTA_ERR_INIT_FAILED    = 7,
} app_ota_err_t;

typedef struct app_ota_status {
    uint8_t  state;            /* app_ota_state_t */
    uint8_t  last_err;         /* app_ota_err_t   */
    uint32_t bytes_received;
    uint32_t total_size;
} app_ota_status_t;

typedef void (*app_ota_status_cb_t)(const app_ota_status_t *s, void *user_arg);

int  app_ota_init        (app_ota_status_cb_t cb, void *user_arg);

int  app_ota_begin       (uint32_t total_size);
int  app_ota_chunk       (uint32_t offset, const void *data, size_t len);
int  app_ota_commit      (const uint8_t *header, size_t header_len);
void app_ota_abort       (void);

void app_ota_get_status  (app_ota_status_t *out);

/* Schedule a NVIC_SystemReset() at +delay_ms so the caller has time to flush
 * the final status notification. delay_ms=0 reboots immediately. */
void app_ota_reboot_after_ms(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif

#endif /* APP_OTA_H */
