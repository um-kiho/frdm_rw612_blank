/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Thin OTA storage backend for Zephyr.
 *
 * This module writes OTA payloads into the MCUboot secondary slot via the
 * Zephyr flash_img API and then requests an upgrade on commit. The higher
 * layers keep using the same transport-agnostic begin/chunk/commit flow.
 */

#include "app_ota.h"

#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "fsl_common.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_ota, LOG_LEVEL_INF);

#ifndef APP_OTA_REBOOT_TASK_STACK
#define APP_OTA_REBOOT_TASK_STACK   1024
#endif

static struct k_mutex       s_lock;
static volatile bool        s_inited;
static app_ota_status_t     s_st;
static app_ota_status_cb_t  s_cb;
static void                *s_cb_arg;
static bool                  s_img_ready;
static uint8_t               s_upload_slot;
static const struct flash_area *s_upload_fa;

#if !DT_NODE_EXISTS(DT_NODELABEL(slot1_partition))
#error "slot1_partition is required for OTA upload target"
#endif

/* ------------------------------------------------------------------------- */
static void emit_status_locked_unlock(void)
{
    /* Snapshot under the lock, release lock, then invoke the callback so the
     * subscriber is free to call back into us (e.g. status read). */
    app_ota_status_t snap = s_st;
    k_mutex_unlock(&s_lock);
    if (s_cb != NULL) s_cb(&snap, s_cb_arg);
}

static int ota_prepare_storage(uint32_t total_size)
{
    int rc = flash_area_open(s_upload_slot, &s_upload_fa);
    if (rc != 0 || s_upload_fa == NULL) {
        LOG_ERR("flash_area_open(slot=%u) failed: %d", (unsigned)s_upload_slot, rc);
        return APP_OTA_ERR_WRITE_FAILED;
    }

    if (total_size > s_upload_fa->fa_size) {
        size_t slot_size = s_upload_fa->fa_size;
        flash_area_close(s_upload_fa);
        s_upload_fa = NULL;
        LOG_ERR("image too large: %u > %u", (unsigned)total_size, (unsigned)slot_size);
        return APP_OTA_ERR_PARAM;
    }

    rc = flash_area_erase(s_upload_fa, 0, s_upload_fa->fa_size);
    if (rc != 0) {
        flash_area_close(s_upload_fa);
        s_upload_fa = NULL;
        LOG_ERR("flash_area_erase failed: %d", rc);
        return APP_OTA_ERR_WRITE_FAILED;
    }

    s_img_ready = true;
    return APP_OTA_OK;
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
        s_upload_slot = FIXED_PARTITION_ID(slot1_partition);
        s_inited = true;
    }

    k_mutex_lock(&s_lock, K_FOREVER);
    s_cb     = cb;
    s_cb_arg = user_arg;
    memset(&s_st, 0, sizeof(s_st));
    s_st.state    = APP_OTA_STATE_IDLE;
    s_st.last_err = APP_OTA_OK;
    s_img_ready   = false;
    s_upload_fa   = NULL;
    k_mutex_unlock(&s_lock);

    LOG_INF("ready");
    return APP_OTA_OK;
}

int app_ota_begin(uint32_t total_size)
{
    if (!s_inited)     return APP_OTA_ERR_INVALID_STATE;
    if (total_size == 0u) return APP_OTA_ERR_PARAM;

    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_upload_fa != NULL) {
        flash_area_close(s_upload_fa);
        s_upload_fa = NULL;
    }
    s_img_ready         = false;
    s_st.bytes_received  = 0u;
    s_st.total_size      = 0u;

    int rc = ota_prepare_storage(total_size);
    if (rc != APP_OTA_OK) {
        s_st.state     = APP_OTA_STATE_FAILED;
        s_st.last_err  = (uint8_t)rc;
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
    if (!s_img_ready)             return APP_OTA_ERR_INVALID_STATE;

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

    /* printk is routed directly to the console (CONFIG_LOG_PRINTK=n in
     * prj.conf), so these markers survive even when the log buffer is
     * saturated. Rate-limit to ~one line per 16 KiB to keep the UART quiet. */
    static uint32_t s_last_marker;
    bool emit = (offset == 0u) ||
                (offset / (16u * 1024u)) != (s_last_marker / (16u * 1024u));
    if (emit) {
        printk("OTA-W: off=%u len=%u BEFORE\n",
               (unsigned)offset, (unsigned)len);
    }
    int rc = flash_area_write(s_upload_fa, (off_t)offset, data, len);
    if (emit) {
        printk("OTA-W: off=%u len=%u AFTER rc=%d\n",
               (unsigned)offset, (unsigned)len, rc);
        s_last_marker = offset;
    }
    if (rc < 0) {
        printk("OTA-W: off=%u len=%u FAIL rc=%d\n",
               (unsigned)offset, (unsigned)len, rc);
        LOG_ERR("flash_area_write failed: %d", rc);
        s_st.state    = APP_OTA_STATE_FAILED;
        s_st.last_err = APP_OTA_ERR_WRITE_FAILED;
        rc = APP_OTA_ERR_WRITE_FAILED;
    } else {
        s_st.bytes_received += (uint32_t)len;
        s_st.state           = APP_OTA_STATE_WRITING;
        s_st.last_err        = APP_OTA_OK;
        rc = APP_OTA_OK;
    }    emit_status_locked_unlock();
    return rc;
}

int app_ota_commit(const uint8_t *header, size_t header_len)
{
    if (!s_inited) return APP_OTA_ERR_INVALID_STATE;
    if (!s_img_ready) return APP_OTA_ERR_INVALID_STATE;
    (void)header;
    (void)header_len;

    k_mutex_lock(&s_lock, K_FOREVER);

    if (s_st.state != APP_OTA_STATE_WRITING) {
        s_st.last_err = APP_OTA_ERR_INVALID_STATE;
        emit_status_locked_unlock();
        return APP_OTA_ERR_INVALID_STATE;
    }
    if (s_st.total_size != 0u &&
        s_st.bytes_received != s_st.total_size) {
        /* The image is incomplete, so keep the staged payload and let the
         * client resume instead of rebooting. */
        s_st.last_err = APP_OTA_ERR_INVALID_OFFSET;
        emit_status_locked_unlock();
        return APP_OTA_ERR_INVALID_OFFSET;
    }

    int rc = APP_OTA_OK;

    s_st.state    = APP_OTA_STATE_COMMITTED;
    s_st.last_err = APP_OTA_OK;
    LOG_INF("commit ok");
    emit_status_locked_unlock();

    app_ota_reboot_after_ms(800u);
    return rc;
}

void app_ota_abort(void)
{
    if (!s_inited) return;
    k_mutex_lock(&s_lock, K_FOREVER);
    s_st.state          = APP_OTA_STATE_IDLE;
    s_st.last_err       = APP_OTA_ERR_ABORTED;
    s_st.bytes_received = 0u;
    s_st.total_size     = 0u;
    if (s_upload_fa != NULL) {
        flash_area_close(s_upload_fa);
        s_upload_fa = NULL;
    }
    s_img_ready         = false;
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
static void reboot_task(void *p1, void *p2, void *p3)
{
    (void)p2;
    (void)p3;
    uint32_t delay_ms = (uint32_t)(uintptr_t)p1;
    if (delay_ms > 0u) {
        k_sleep(K_MSEC(delay_ms));
    }
    LOG_INF("reboot now");
#if defined(CONFIG_REBOOT)
    sys_reboot(SYS_REBOOT_COLD);
#else
    NVIC_SystemReset();
#endif
}

void app_ota_reboot_after_ms(uint32_t delay_ms)
{
    static K_THREAD_STACK_DEFINE(reboot_task_stack, APP_OTA_REBOOT_TASK_STACK);
    static struct k_thread reboot_task_data;
    /* Idempotent: app_ota_commit() and the HTTP auto-commit path both request a
     * reboot. Recreating the same static k_thread while the first reboot thread
     * is still sleeping corrupts it, so the board never actually reboots. Latch
     * the first request and ignore the rest. */
    static atomic_t s_reboot_scheduled = ATOMIC_INIT(0);
    if (atomic_set(&s_reboot_scheduled, 1) != 0) {
        return;
    }
    k_thread_create(&reboot_task_data, reboot_task_stack,
                    K_THREAD_STACK_SIZEOF(reboot_task_stack),
                    reboot_task, (void *)(uintptr_t)delay_ms, NULL, NULL,
                    K_HIGHEST_THREAD_PRIO, 0, K_NO_WAIT);
}
