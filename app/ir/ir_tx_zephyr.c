/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Zephyr IR TX backend: SCTimer PWM (channel 5 = SCT_OUT5 = GPIO27)
 *
 * 38 kHz 캐리어 생성: Zephyr PWM API → SCTimer
 * 마크/스페이스 타이밍: k_busy_wait (µs 단위 정밀도)
 * 전송 스레드: BLE/앱 컨텍스트 블로킹 없이 독립 실행
 *
 * GPIO26(SCT_OUT4)은 FC3 USART 콘솔과 충돌 → GPIO27(SCT_OUT5) 사용
 *  carrier_on()  → pwm duty 33 % (SCT_OUT5 = GPIO27)
 *  carrier_off() → pwm duty 0   (GPIO27 → LOW)
 */

#include "ir_tx.h"

#include <string.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_ir_tx, LOG_LEVEL_INF);

/* SCTimer PWM 채널 5 = SCT_OUT5 = GPIO27 (GPIO26은 FC3 콘솔 UART와 충돌) */
#define IR_PWM_DEV      DEVICE_DT_GET(DT_NODELABEL(sctimer))
#define IR_PWM_CHANNEL  5u

#ifndef IR_THREAD_STACK
#define IR_THREAD_STACK 2048
#endif
#ifndef IR_THREAD_PRIO
#define IR_THREAD_PRIO  2       /* 높은 우선순위 — 타이밍 정밀도 확보 */
#endif

/* ---------- 모듈 상태 ---------- */
static const struct device *s_pwm;
static volatile bool        s_inited;
static uint32_t             s_carrier_hz = APP_IR_TX_DEFAULT_CARRIER_HZ;

static struct k_mutex       s_busy_mtx;
static struct k_sem         s_trigger;   /* 전송 요청 신호 */
static struct k_sem         s_done;      /* 전송 완료 신호 */

static ir_symbol_t          s_buf[APP_IR_TX_MAX_SYMBOLS];
static volatile size_t      s_count;

/* ---------- 캐리어 제어 ---------- */
static inline void carrier_on(void)
{
    uint32_t period_ns = NSEC_PER_SEC / s_carrier_hz;
    uint32_t pulse_ns  = period_ns / 3u;    /* 33 % duty */
    (void)pwm_set(s_pwm, IR_PWM_CHANNEL, period_ns, pulse_ns,
                  PWM_POLARITY_NORMAL);
}

static inline void carrier_off(void)
{
    uint32_t period_ns = NSEC_PER_SEC / s_carrier_hz;
    (void)pwm_set(s_pwm, IR_PWM_CHANNEL, period_ns, 0u,
                  PWM_POLARITY_NORMAL);
}

/* ---------- 전송 스레드 ---------- */
static K_THREAD_STACK_DEFINE(s_stack, IR_THREAD_STACK);
static struct k_thread s_thread_data;

static void ir_tx_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&s_trigger, K_FOREVER);

        size_t cnt = s_count;

        for (size_t i = 0; i < cnt; i++) {
            carrier_on();
            k_busy_wait(s_buf[i].mark_us);
            carrier_off();
            k_busy_wait(s_buf[i].space_us);
        }

        carrier_off();      /* 마지막 스페이스 후 확실히 LOW */
        k_sem_give(&s_done);
    }
}

/* ---------- 공개 API ---------- */
int ir_tx_init(void)
{
    if (s_inited) {
        return 0;
    }

    s_pwm = IR_PWM_DEV;
    if (!device_is_ready(s_pwm)) {
        LOG_ERR("SCTimer PWM not ready");
        return -ENODEV;
    }

    k_mutex_init(&s_busy_mtx);
    k_sem_init(&s_trigger, 0, 1);
    k_sem_init(&s_done,    0, 1);

    carrier_off();

    k_tid_t tid = k_thread_create(&s_thread_data, s_stack,
                                   K_THREAD_STACK_SIZEOF(s_stack),
                                   ir_tx_thread, NULL, NULL, NULL,
                                   IR_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(tid, "ir_tx");

    s_inited = true;
    LOG_INF("ready  carrier=%u Hz  pin=GPIO26(SCT_OUT4)", s_carrier_hz);
    return 0;
}

void ir_tx_deinit(void)
{
    s_inited = false;
}

bool ir_tx_is_ready(void)
{
    return s_inited;
}

int ir_tx_set_carrier_hz(uint32_t hz)
{
    if (hz < 25000u || hz > 60000u) {
        return -EINVAL;
    }
    s_carrier_hz = hz;
    return 0;
}

uint32_t ir_tx_get_carrier_hz(void)
{
    return s_carrier_hz;
}

int ir_tx_send_symbols(const ir_symbol_t *syms, size_t count)
{
    if (!s_inited)                       return -EAGAIN;
    if (syms == NULL || count == 0u)     return -EINVAL;
    if (count > APP_IR_TX_MAX_SYMBOLS)   return -ENOMEM;

    k_mutex_lock(&s_busy_mtx, K_FOREVER);

    memcpy(s_buf, syms, count * sizeof(ir_symbol_t));
    s_count = count;

    /* 이전 done 신호 소비 후 전송 트리거 */
    (void)k_sem_take(&s_done, K_NO_WAIT);
    k_sem_give(&s_trigger);

    /* 전송 완료 대기 (최대 3초) */
    int ret = k_sem_take(&s_done, K_MSEC(APP_IR_TX_SEND_TIMEOUT_MS));

    k_mutex_unlock(&s_busy_mtx);

    if (ret != 0) {
        LOG_ERR("send timeout");
        return -ETIMEDOUT;
    }
    return 0;
}
