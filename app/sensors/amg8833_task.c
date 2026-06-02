/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "amg8833_task.h"

#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(amg_task, LOG_LEVEL_INF);

#ifndef AMG_TASK_STACK
#define AMG_TASK_STACK          2048
#endif
#ifndef AMG_TASK_PRIO
#define AMG_TASK_PRIO           2
#endif
#ifndef AMG_TASK_MIN_PERIOD_MS
#define AMG_TASK_MIN_PERIOD_MS  100u
#endif
#define AMG_PROBE_TIMEOUT_MS    2000u   /* I2C hang 감지 watchdog */

static amg8833_t          s_dev;
static K_MUTEX_DEFINE(s_lock);
static volatile bool      s_running;
static volatile bool      s_probing;
static volatile bool      s_has_frame;
static amg_sample_t       s_last;
static amg_sample_cb_t    s_cb;
static void              *s_cb_arg;
static uint32_t           s_period_ms;
static uint8_t            s_addr;
static k_tid_t            s_tid;

static struct k_work_delayable s_watchdog_dwork;

static void watchdog_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    if (s_probing && s_tid != NULL) {
        LOG_WRN("AMG8833 I2C timeout — no hardware, aborting task");
        k_thread_abort(s_tid);
        s_running = false;
        s_tid     = NULL;
    }
}

static void compute_stats(amg_sample_t *s)
{
    int32_t sum = 0;
    int16_t mn  =  INT16_MAX;
    int16_t mx  =  INT16_MIN;
    for (int i = 0; i < AMG8833_PIXELS; ++i) {
        int16_t v = s->pixels_q2[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    s->min_q2 = mn;
    s->max_q2 = mx;
    s->avg_q2 = (int16_t)(sum / AMG8833_PIXELS);
}

static void publish(const amg_sample_t *s)
{
    k_mutex_lock(&s_lock, K_FOREVER);
    s_last      = *s;
    s_has_frame = true;
    k_mutex_unlock(&s_lock);

    if (s_cb != NULL) {
        s_cb(s, s_cb_arg);
    }
}

static void amg_task(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;
    uint16_t seq = 0u;
    uint32_t next_time = k_uptime_get_32();

    k_sleep(K_MSEC(300));
    LOG_INF("probing I2C addr=0x%02X (watchdog=%u ms)...", s_addr, AMG_PROBE_TIMEOUT_MS);

    s_probing = true;
    int last_rc = amg8833_init(&s_dev, s_addr);
    s_probing = false;
    k_work_cancel_delayable(&s_watchdog_dwork);

    if (last_rc == AMG8833_OK) {
        LOG_INF("AMG8833 ready (addr=0x%02X)", s_addr);
    } else {
        LOG_WRN("AMG8833 not found (addr=0x%02X rc=%d)", s_addr, last_rc);
    }

    while (s_running) {
        amg_sample_t s;
        memset(&s, 0, sizeof(s));

        if (last_rc != AMG8833_OK) {
            last_rc = amg8833_init(&s_dev, s_addr);
        }

        if (last_rc == AMG8833_OK) {
            int rc = amg8833_read_frame(&s_dev);
            if (rc == AMG8833_OK) {
                memcpy(s.pixels_q2, s_dev.pixels_q2, sizeof(s.pixels_q2));
                compute_stats(&s);
                (void)amg8833_read_thermistor(&s_dev);
                s.thermistor_q4 = s_dev.thermistor_q4;
                s.err           = 0;
            } else {
                s.err   = (int8_t)rc;
                last_rc = rc;
            }
        } else {
            s.err = (int8_t)last_rc;
        }

        s.seq          = ++seq;
        s.timestamp_ms = k_uptime_get_32();

        publish(&s);

        next_time += s_period_ms;
        k_sleep(K_TIMEOUT_ABS_MS(next_time));
    }
}

int amg_task_start(uint8_t addr_7b, uint32_t period_ms,
                   amg_sample_cb_t cb, void *user_arg)
{
    if (addr_7b != AMG8833_ADDR_LOW && addr_7b != AMG8833_ADDR_HIGH) {
        return -1;
    }
    if (period_ms < AMG_TASK_MIN_PERIOD_MS) {
        period_ms = AMG_TASK_MIN_PERIOD_MS;
    }
    if (s_running) return 0;

    s_addr      = addr_7b;
    s_cb        = cb;
    s_cb_arg    = user_arg;
    s_period_ms = period_ms;
    s_running   = true;
    s_probing   = false;

    static K_THREAD_STACK_DEFINE(amg_task_stack, AMG_TASK_STACK);
    static struct k_thread amg_task_data;
    s_tid = k_thread_create(&amg_task_data, amg_task_stack,
                            K_THREAD_STACK_SIZEOF(amg_task_stack),
                            amg_task, NULL, NULL, NULL,
                            AMG_TASK_PRIO, 0, K_NO_WAIT);
    if (s_tid == NULL) {
        s_running = false;
        return -3;
    }
    k_thread_name_set(s_tid, "zcube_amg");

    k_work_init_delayable(&s_watchdog_dwork, watchdog_fn);
    k_work_schedule(&s_watchdog_dwork, K_MSEC(AMG_PROBE_TIMEOUT_MS));

    return 0;
}

void amg_task_stop(void)
{
    s_running = false;
}

bool amg_task_is_running(void)
{
    return s_running;
}

int amg_task_get_last_frame(amg_sample_t *out)
{
    if (out == NULL) return -1;
    if (!s_has_frame) return -1;
    k_mutex_lock(&s_lock, K_FOREVER);
    *out = s_last;
    k_mutex_unlock(&s_lock);
    return 0;
}
