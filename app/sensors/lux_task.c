/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lux_task.h"
#include "bh1750.h"
#include "bh1750_io_nxp_i2c.h"
#include "i2c_bus.h"

#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lux_task, LOG_LEVEL_INF);

#ifndef LUX_TASK_STACK
#define LUX_TASK_STACK         1024
#endif
#ifndef LUX_TASK_PRIO
#define LUX_TASK_PRIO          2
#endif
#ifndef LUX_TASK_MIN_PERIOD_MS
#define LUX_TASK_MIN_PERIOD_MS 200u
#endif
#define LUX_PROBE_TIMEOUT_MS   2500u   /* watchdog: 200ms sleep + 2×(100ms probe) + 여유 */
#define LUX_PROBE_INTER_MS     20u     /* FlexComm bus-busy 해소 대기 (probe 사이) */

static bh1750_io_t        s_io;
static bh1750_t           s_s1;        /* 0x23, ADDR=LOW  */
static bh1750_t           s_s2;        /* 0x5C, ADDR=HIGH */
static K_MUTEX_DEFINE(s_lock);
static volatile bool      s_running;
static volatile bool      s_probing;   /* hw_init 진행 중 플래그 */
static volatile bool      s_has_sample;
static lux_sample_t       s_last;
static lux_sample_cb_t    s_cb;
static void              *s_cb_arg;
static uint32_t           s_period_ms;
static k_tid_t            s_tid;

static struct k_work_delayable s_watchdog_dwork;

static void watchdog_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    if (s_probing && s_tid != NULL) {
        LOG_WRN("BH1750 I2C timeout — no hardware, aborting task");
        k_thread_abort(s_tid);
        i2c_bus_force_unlock();   /* abort 시 뮤텍스가 잠긴 채 남지 않도록 재초기화 */
        s_running = false;
        s_tid     = NULL;
    }
}

static int hw_init(void)
{
	LOG_INF("BH1750 check");
    int rc = bh1750_io_nxp_i2c_init(&s_io);
    if (rc != BH1750_OK) {
        LOG_ERR("bh1750_io init rc=%d", rc);
        return rc;
    }

    /* ── 버스 진단: 온보드 P3T1755(0x48) probe ──────────────────────────
     * 같은 FC2 I2C 버스에 보드 기본 온도센서가 0x48에 항상 존재한다.
     * - 0x48 found(빠름): 버스/페리페럴 정상 → 0x5C/0x23 미검출은 센서 미연결/배선 문제
     * - 0x48 100ms 타임아웃/hang: 버스가 처음부터 stuck (SDA/SCL hold, 쇼트, 배선)
     * 정상 장치를 첫 트랜잭션으로 보내 버스를 깨끗한 상태로 만드는 효과도 있다. */
    LOG_INF("probe 0x48 (onboard P3T1755)...");
    bool found_ref = (i2c_bus_probe(0x48) == 0);
    LOG_INF("probe 0x48: %s", found_ref ? "found (bus OK)" : "NOT found (bus stuck?)");
    k_sleep(K_MSEC(LUX_PROBE_INTER_MS));

    /* 실제 I2C 통신 전에 장치 존재 여부 확인 (타임아웃 내 빠른 실패) */
    LOG_INF("probe 0x%02x...", BH1750_ADDR_HIGH);
    bool found1 = (i2c_bus_probe(BH1750_ADDR_HIGH)  == 0);
    LOG_INF("probe 0x%02x: %s", BH1750_ADDR_HIGH,  found1 ? "found" : "not found");

    /* NXP FlexComm: 이전 probe 후 bus-busy 해소 대기 */
    k_sleep(K_MSEC(LUX_PROBE_INTER_MS));

    LOG_INF("probe 0x%02x...", BH1750_ADDR_LOW);
    bool found2 = (i2c_bus_probe(BH1750_ADDR_LOW) == 0);
    LOG_INF("probe 0x%02x: %s", BH1750_ADDR_LOW, found2 ? "found" : "not found");

    if (!found1 && !found2) {
        LOG_ERR("bh1750 find= error");
        return BH1750_ERR_IO;
    }

    /* 프로브 성공 → 하드웨어 확인됨, 워치독 불필요 */
    k_work_cancel_delayable(&s_watchdog_dwork);
    LOG_INF("k_work_cancel_delayable");
    /* bh1750_init()은 I2C 통신 없이 struct만 초기화 */
    rc = bh1750_init(&s_s1, &s_io, BH1750_ADDR_LOW);
    if (rc != BH1750_OK) { LOG_ERR("s1 init rc=%d", rc); return rc; }
    rc = bh1750_init(&s_s2, &s_io, BH1750_ADDR_HIGH);
    if (rc != BH1750_OK) { LOG_ERR("s2 init rc=%d", rc); return rc; }

    if (found1) {
        rc = bh1750_set_mode(&s_s1, BH1750_MODE_CONT_H);
        if (rc != BH1750_OK) { LOG_ERR("s1 set_mode rc=%d", rc); return rc; }
        LOG_INF("BH1750 0x23 ready");
    }
    if (found2) {
        rc = bh1750_set_mode(&s_s2, BH1750_MODE_CONT_H);
        if (rc != BH1750_OK) { LOG_ERR("s2 set_mode rc=%d", rc); return rc; }
        LOG_INF("BH1750 0x5C ready");
    }

    return BH1750_OK;
}

static void publish(const lux_sample_t *s)
{
    k_mutex_lock(&s_lock, K_FOREVER);
    s_last       = *s;
    s_has_sample = true;
    k_mutex_unlock(&s_lock);

    if (s_cb != NULL) {
        s_cb(s, s_cb_arg);
    }
}

static void lux_task(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;
    uint16_t seq = 0u;
    uint32_t next_time = k_uptime_get_32();

    k_sleep(K_MSEC(200));
    LOG_INF("probing I2C (watchdog=%u ms)...", (uint32_t)LUX_PROBE_TIMEOUT_MS);

    s_probing = true;
    int init_rc = hw_init();
    s_probing = false;
    k_work_cancel_delayable(&s_watchdog_dwork);

    if (init_rc == BH1750_OK) {
        LOG_INF("BH1750 ready: 2 sensors (0x23, 0x5C)");
    } else {
        LOG_WRN("BH1750 not found (rc=%d)", init_rc);
    }

    while (s_running) {
        lux_sample_t s = {0};

        s.err1 = (int8_t)bh1750_read_lux_x100(&s_s1, &s.lux1_x100);
        s.err2 = (int8_t)bh1750_read_lux_x100(&s_s2, &s.lux2_x100);
        s.seq          = ++seq;
        s.timestamp_ms = k_uptime_get_32();

        publish(&s);

        next_time += s_period_ms;
        k_sleep(K_TIMEOUT_ABS_MS(next_time));
    }
}

int lux_task_start(uint32_t period_ms,
                   lux_sample_cb_t cb, void *user_arg)
{
    if (period_ms < LUX_TASK_MIN_PERIOD_MS) {
        period_ms = LUX_TASK_MIN_PERIOD_MS;
    }
    if (s_running) return 0;

    s_cb        = cb;
    s_cb_arg    = user_arg;
    s_period_ms = period_ms;
    s_running   = true;
    s_probing   = false;

    static K_THREAD_STACK_DEFINE(lux_task_stack, LUX_TASK_STACK);
    static struct k_thread lux_task_data;
    s_tid = k_thread_create(&lux_task_data, lux_task_stack,
                            K_THREAD_STACK_SIZEOF(lux_task_stack),
                            lux_task, NULL, NULL, NULL,
                            LUX_TASK_PRIO, 0, K_NO_WAIT);
    if (s_tid == NULL) {
        s_running = false;
        return -2;
    }
    k_thread_name_set(s_tid, "zcube_lux");

    k_work_init_delayable(&s_watchdog_dwork, watchdog_fn);
    k_work_schedule(&s_watchdog_dwork, K_MSEC(LUX_PROBE_TIMEOUT_MS));

    return 0;
}

void lux_task_stop(void)
{
    s_running = false;
}

bool lux_task_is_running(void)
{
    return s_running;
}

int lux_task_get_last(lux_sample_t *out)
{
    if (out == NULL) return -1;
    if (!s_has_sample) return -1;
    k_mutex_lock(&s_lock, K_FOREVER);
    *out = s_last;
    k_mutex_unlock(&s_lock);
    return 0;
}
