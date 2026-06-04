/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "amg8833_task.h"
#include "i2c_bus.h"

#include <string.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(amg_task, LOG_LEVEL_INF);

#ifndef AMG_TASK_STACK
#define AMG_TASK_STACK          4096   /* read_frame()이 128B 프레임 버퍼를 스택에 잡음 */
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
        /* I2C 가 더 이상 hang 하지 않으므로(미연결=즉시 NAK) 이 워치독은 보호용
         * 잔재다. k_thread_abort 는 I2C 락을 영구 잠가 전체 데드락을 유발하므로
         * 절대 쓰지 않는다. s_running 만 내려 태스크가 다음 루프에서 정상 종료. */
        LOG_WRN("AMG8833 I2C timeout — stopping task (no abort)");
        s_running = false;
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
    /* lux_task 와 동일한 탐지 방식: 실제 통신(레지스터 write) 전에 i2c_bus_probe()
     * (0바이트 write = 빠른 ACK 확인)로 장치 존재부터 확인한다. */
    bool found = (i2c_bus_probe(s_addr) == 0);
    int last_rc = found ? amg8833_init(&s_dev, s_addr) : AMG8833_ERR_IO;
    s_probing = false;
    k_work_cancel_delayable(&s_watchdog_dwork);

    if (!found || last_rc != AMG8833_OK) {
        /* 미응답/미연결 센서를 계속 폴링하면 실패 I2C 전송(NAK + 드라이버의
         * I2C_MasterTransferAbort)이 WiFi 연결 임계구간과 겹쳐 시스템 전체가
         * 멈춘다. 못 찾으면 태스크를 종료해 추가 I2C 트래픽을 만들지 않는다.
         * (센서 연결 후에는 재부팅/재시작으로 다시 탐지) */
        LOG_WRN("AMG8833 not found (addr=0x%02X rc=%d) — stopping (no I2C retry)",
                s_addr, last_rc);
        s_running = false;
        s_tid     = NULL;
        return;
    }
    LOG_INF("AMG8833 ready (addr=0x%02X)", s_addr);

    while (s_running) {
        amg_sample_t s;
        memset(&s, 0, sizeof(s));

        /* 센서가 한번 확인된 뒤에는 read 만 한다. read 실패해도 재init 하지
         * 않는다(실패 I2C 폭주 방지). 실패한 전송은 i2c_bus 가 엔진을 재래치해
         * 다음 전송이 hang 하지 않게 복구한다. */
        int rc = amg8833_read_frame(&s_dev);
        if (rc == AMG8833_OK) {
            memcpy(s.pixels_q2, s_dev.pixels_q2, sizeof(s.pixels_q2));
            compute_stats(&s);
            (void)amg8833_read_thermistor(&s_dev);
            s.thermistor_q4 = s_dev.thermistor_q4;
            s.err           = 0;
        } else {
            s.err = (int8_t)rc;
        }

        s.seq          = ++seq;
        s.timestamp_ms = k_uptime_get_32();

        publish(&s);

        next_time += s_period_ms;
        uint32_t now = k_uptime_get_32();
        if ((int32_t)(next_time - now) < 1) {
            /* 처리시간이 주기를 초과해 일정이 밀리면 절대시각 sleep 이 0/음수가
             * 되어 yield 없이 tight-loop → CPU 독점(네트워크·로그 starvation,
             * "다운"처럼 보임). 일정을 리셋해 항상 최소 한 주기를 sleep 한다. */
            next_time = now + s_period_ms;
        }
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
