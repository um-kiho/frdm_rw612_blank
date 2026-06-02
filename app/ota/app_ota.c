/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Thin wrapper over NXP wireless-framework OtaSupport. Drives the slot1
 * staging area and lets multiple transports (BLE / TCP / ...) feed image
 * bytes through one serialised state machine.
 *
 * NXP API used (from middleware/wireless/framework/OtaSupport/Interface/OtaSupport.h):
 *
 *   uint8_t      OTA_ClientInit       (void);
 *   otaResult_t  OTA_StartImage       (uint32_t length);
 *   otaResult_t  OTA_PushImageChunk   (uint8_t *data, uint16_t length,
 *                                      uint32_t *pImageOffset,
 *                                      uint16_t *pImageLength);
 *   otaResult_t  OTA_CommitImage      (uint8_t *pHeader);
 *   void         OTA_CancelImage      (void);
 *
 * gOtaSuccess_c == 0 in the same header.
 */

#include "app_ota.h"

#include <string.h>
#include <zephyr/kernel.h>

#include "fsl_common.h"          /* NVIC_SystemReset */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_ota, LOG_LEVEL_INF);

#include "OtaSupport.h"

#ifndef APP_OTA_REBOOT_TASK_STACK
#define APP_OTA_REBOOT_TASK_STACK   1024
#endif

static struct k_mutex       s_lock;
static volatile bool        s_inited;
static app_ota_status_t     s_st;
static app_ota_status_cb_t  s_cb;
static void                *s_cb_arg;

/* ------------------------------------------------------------------------- */
static void emit_status_locked_unlock(void)
{
    /* Snapshot under the lock, release lock, then invoke the callback so the
     * subscriber is free to call back into us (e.g. status read). */
    app_ota_status_t snap = s_st;
    k_mutex_unlock(&s_lock);
    if (s_cb != NULL) s_cb(&snap, s_cb_arg);
}

static int map_ota(otaResult_t r)
{
    return (r == gOtaSuccess_c) ? APP_OTA_OK : APP_OTA_ERR_WRITE_FAILED;
}

/* ------------------------------------------------------------------------- */
int app_ota_init(app_ota_status_cb_t cb, void *user_arg)
{
    static bool mutex_inited = false;
    if (!mutex_inited) {
        k_mutex_init(&s_lock);
        mutex_inited = true;
    }

    if (!s_inited) {
        if (OTA_ClientInit() != 0u) {
            LOG_ERR("OTA_ClientInit failed");
            return APP_OTA_ERR_INIT_FAILED;
        }
        s_inited = true;
    }

    k_mutex_lock(&s_lock, K_FOREVER);
    s_cb     = cb;
    s_cb_arg = user_arg;
    memset(&s_st, 0, sizeof(s_st));
    s_st.state    = APP_OTA_STATE_IDLE;
    s_st.last_err = APP_OTA_OK;
    k_mutex_unlock(&s_lock);

    LOG_INF("ready");
    return APP_OTA_OK;
}

int app_ota_begin(uint32_t total_size)
{
    if (!s_inited)     return APP_OTA_ERR_INVALID_STATE;
    if (total_size == 0u) return APP_OTA_ERR_PARAM;

    k_mutex_lock(&s_lock, K_FOREVER);

    /* Restart cleanly if a previous session was left mid-way. */
    if (s_st.state == APP_OTA_STATE_READY   ||
        s_st.state == APP_OTA_STATE_WRITING ||
        s_st.state == APP_OTA_STATE_FAILED) {
        OTA_CancelImage();
    }

    otaResult_t r = OTA_StartImage(total_size);
    int rc;
    if (r != gOtaSuccess_c) {
        s_st.state     = APP_OTA_STATE_FAILED;
        s_st.last_err  = APP_OTA_ERR_WRITE_FAILED;
        rc = APP_OTA_ERR_WRITE_FAILED;
    } else {
        s_st.state          = APP_OTA_STATE_READY;
        s_st.last_err       = APP_OTA_OK;
        s_st.bytes_received = 0u;
        s_st.total_size     = total_size;
        rc = APP_OTA_OK;
    }
    LOG_INF("begin total=%u rc=%d", (unsigned)total_size, rc);
    emit_status_locked_unlock();
    return rc;
}

int app_ota_chunk(uint32_t offset, const void *data, size_t len)
{
    if (!s_inited)                return APP_OTA_ERR_INVALID_STATE;
    if (data == NULL || len == 0u) return APP_OTA_ERR_PARAM;
    if (len > UINT16_MAX)         return APP_OTA_ERR_PARAM;

    k_mutex_lock(&s_lock, K_FOREVER);

    if (s_st.state != APP_OTA_STATE_READY &&
        s_st.state != APP_OTA_STATE_WRITING) {
        s_st.last_err = APP_OTA_ERR_INVALID_STATE;
        emit_status_locked_unlock();
        return APP_OTA_ERR_INVALID_STATE;
    }

    /* Strict in-order policy: re-send is allowed (offset matches current
     * position), forward jumps are rejected so the upper layer cannot leave
     * holes in the staged image. */
    if (offset != s_st.bytes_received) {
        s_st.last_err = APP_OTA_ERR_INVALID_OFFSET;
        emit_status_locked_unlock();
        return APP_OTA_ERR_INVALID_OFFSET;
    }
    if (s_st.total_size != 0u &&
        (uint64_t)offset + len > (uint64_t)s_st.total_size) {
        s_st.state    = APP_OTA_STATE_FAILED;
        s_st.last_err = APP_OTA_ERR_PARAM;
        emit_status_locked_unlock();
        return APP_OTA_ERR_PARAM;
    }

    uint32_t pos  = offset;
    uint16_t wrote = 0;
    otaResult_t r = OTA_PushImageChunk((uint8_t *)data, (uint16_t)len,
                                       &pos, &wrote);
    int rc;
    if (r != gOtaSuccess_c) {
        s_st.state    = APP_OTA_STATE_FAILED;
        s_st.last_err = APP_OTA_ERR_WRITE_FAILED;
        rc = APP_OTA_ERR_WRITE_FAILED;
    } else {
        s_st.bytes_received += (wrote != 0u) ? wrote : (uint32_t)len;
        s_st.state           = APP_OTA_STATE_WRITING;
        s_st.last_err        = APP_OTA_OK;
        rc = APP_OTA_OK;
    }
    emit_status_locked_unlock();
    return rc;
}

int app_ota_commit(const uint8_t *header, size_t header_len)
{
    (void)header_len;
    if (!s_inited) return APP_OTA_ERR_INVALID_STATE;

    k_mutex_lock(&s_lock, K_FOREVER);

    if (s_st.state != APP_OTA_STATE_WRITING) {
        s_st.last_err = APP_OTA_ERR_INVALID_STATE;
        emit_status_locked_unlock();
        return APP_OTA_ERR_INVALID_STATE;
    }
    if (s_st.total_size != 0u &&
        s_st.bytes_received != s_st.total_size) {
        /* NXP OTA_CommitImage will reject too, but bail earlier with a
         * descriptive error so the client can resume. */
        s_st.last_err = APP_OTA_ERR_INVALID_OFFSET;
        emit_status_locked_unlock();
        return APP_OTA_ERR_INVALID_OFFSET;
    }

    otaResult_t r = OTA_CommitImage((uint8_t *)header);
    int rc = map_ota(r);
    if (rc == APP_OTA_OK) {
        s_st.state    = APP_OTA_STATE_COMMITTED;
        s_st.last_err = APP_OTA_OK;
    } else {
        s_st.state    = APP_OTA_STATE_FAILED;
        s_st.last_err = APP_OTA_ERR_COMMIT_FAILED;
        rc = APP_OTA_ERR_COMMIT_FAILED;
    }
    LOG_INF("commit rc=%d", rc);
    emit_status_locked_unlock();
    return rc;
}

void app_ota_abort(void)
{
    if (!s_inited) return;
    k_mutex_lock(&s_lock, K_FOREVER);
    OTA_CancelImage();
    s_st.state          = APP_OTA_STATE_IDLE;
    s_st.last_err       = APP_OTA_ERR_ABORTED;
    s_st.bytes_received = 0u;
    s_st.total_size     = 0u;
    LOG_INF("aborted");
    emit_status_locked_unlock();
}

void app_ota_get_status(app_ota_status_t *out)
{
    if (out == NULL) return;
    k_mutex_lock(&s_lock, K_FOREVER);
    *out = s_st;
    k_mutex_unlock(&s_lock);
}

/* ------------------------------------------------------------------------- */
static void reboot_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    if (delay_ms > 0u) {
        k_sleep(K_MSEC(delay_ms));
    }
    LOG_INF("reboot now");
    NVIC_SystemReset();
}

void app_ota_reboot_after_ms(uint32_t delay_ms)
{
    static K_THREAD_STACK_DEFINE(reboot_task_stack, APP_OTA_REBOOT_TASK_STACK);
    static struct k_thread reboot_task_data;
    k_thread_create(&reboot_task_data, reboot_task_stack,
                    K_THREAD_STACK_SIZEOF(reboot_task_stack),
                    reboot_task, (void *)(uintptr_t)delay_ms, NULL, NULL,
                    K_HIGHEST_THREAD_PRIO, 0, K_NO_WAIT);
}
