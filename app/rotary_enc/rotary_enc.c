/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * EC16 로터리 인코더 Zephyr 포트
 *
 * 핀: GPIO4(ROT_A), GPIO5(ROT_B) — overlay alias rot-a / rot-b
 * 쿼드러처 룩업테이블: (old_state << 2) | new_state → -1 / 0 / +1
 */

#include "rotary_enc.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rotary_enc, LOG_LEVEL_INF);

/* DT alias에서 GPIO 스펙 추출 */
#define ROT_A_NODE  DT_ALIAS(rot_a)
#define ROT_B_NODE  DT_ALIAS(rot_b)
#define ROT_SW_NODE DT_ALIAS(rot_sw)

static const struct gpio_dt_spec s_pin_a =
    GPIO_DT_SPEC_GET(ROT_A_NODE, gpios);
static const struct gpio_dt_spec s_pin_b =
    GPIO_DT_SPEC_GET(ROT_B_NODE, gpios);
static const struct gpio_dt_spec s_pin_sw =
    GPIO_DT_SPEC_GET(ROT_SW_NODE, gpios);

#define ROT_SW_DEBOUNCE_MS  30u

/* 쿼드러처 전이 룩업테이블: state = (A<<1)|B */
static const int8_t s_quad[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static atomic_t           s_count;
static uint8_t            s_state;
static struct gpio_callback s_cb_a;
static struct gpio_callback s_cb_b;

static atomic_t           s_sw_count;     /* 버튼 누름 횟수 */
static struct k_work_delayable s_sw_poll; /* 버튼은 인터럽트 대신 폴링 */
static bool               s_sw_prev;      /* 직전 눌림 상태(edge 검출) */
#define ROT_SW_POLL_MS    20u

/* ── 공통 ISR ───────────────────────────────────────────────────────── */
static void rotary_isr(const struct device *dev, struct gpio_callback *cb,
                       uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    int a = gpio_pin_get_dt(&s_pin_a);
    int b = gpio_pin_get_dt(&s_pin_b);

    uint8_t new_st = (uint8_t)(((a & 1) << 1) | (b & 1));
    uint8_t idx    = (uint8_t)((s_state << 2) | new_st);
    int8_t  delta  = s_quad[idx];

    s_state = new_st;

    if (delta != 0) {
        atomic_add(&s_count, (atomic_val_t)delta);
    }
}

/* ── SW(푸시 버튼) 폴링 — 인터럽트 storm 방지. 20ms 주기로 눌림 edge 검출 ── */
static void rotary_sw_poll_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    /* gpio_pin_get_dt: active-low 반영 → 눌림(라인 LOW)=1 */
    int logical = gpio_pin_get_dt(&s_pin_sw);
    int raw     = gpio_pin_get_raw(s_pin_sw.port, s_pin_sw.pin);
    bool pressed = (logical == 1);
    if (pressed != s_sw_prev) {
        /* 진단: 레벨 전이 시 logical/raw 출력 (눌렀을 때 바뀌는지 확인) */
        printf("ROT: SW edge logical=%d raw=%d -> %s\n",
               logical, raw, pressed ? "DOWN" : "up");
        if (pressed) {
            atomic_inc(&s_sw_count);
        }
    }
    s_sw_prev = pressed;
    k_work_reschedule(&s_sw_poll, K_MSEC(ROT_SW_POLL_MS));
}

/* ── 공개 API ───────────────────────────────────────────────────────── */

int rotary_enc_init(void)
{
    if (!gpio_is_ready_dt(&s_pin_a) || !gpio_is_ready_dt(&s_pin_b)) {
        LOG_ERR("GPIO device not ready");
        return -ENODEV;
    }

    atomic_set(&s_count, 0);

    int rc;

    rc = gpio_pin_configure_dt(&s_pin_a, GPIO_INPUT);
    if (rc != 0) {
        LOG_ERR("ROT_A configure failed: %d", rc);
        return rc;
    }

    rc = gpio_pin_configure_dt(&s_pin_b, GPIO_INPUT);
    if (rc != 0) {
        LOG_ERR("ROT_B configure failed: %d", rc);
        return rc;
    }

    /* 초기 상태 */
    int a = gpio_pin_get_dt(&s_pin_a);
    int b = gpio_pin_get_dt(&s_pin_b);

    s_state = (uint8_t)(((a & 1) << 1) | (b & 1));

    /* ANY_EDGE 인터럽트 */
    rc = gpio_pin_interrupt_configure_dt(&s_pin_a, GPIO_INT_EDGE_BOTH);
    if (rc != 0) {
        LOG_ERR("ROT_A interrupt configure failed: %d", rc);
        return rc;
    }

    rc = gpio_pin_interrupt_configure_dt(&s_pin_b, GPIO_INT_EDGE_BOTH);
    if (rc != 0) {
        LOG_ERR("ROT_B interrupt configure failed: %d", rc);
        return rc;
    }

    gpio_init_callback(&s_cb_a, rotary_isr, BIT(s_pin_a.pin));
    rc = gpio_add_callback(s_pin_a.port, &s_cb_a);
    if (rc != 0) {
        LOG_ERR("add_callback ROT_A failed: %d", rc);
        return rc;
    }

    gpio_init_callback(&s_cb_b, rotary_isr, BIT(s_pin_b.pin));
    rc = gpio_add_callback(s_pin_b.port, &s_cb_b);
    if (rc != 0) {
        LOG_ERR("add_callback ROT_B failed: %d", rc);
        return rc;
    }

    /* ── SW(푸시 버튼, GPIO11, active-low) — 인터럽트 없이 폴링 ───────────── */
    atomic_set(&s_sw_count, 0);
    s_sw_prev = false;
    if (!gpio_is_ready_dt(&s_pin_sw)) {
        LOG_WRN("ROT_SW not ready — 버튼 비활성");
    } else {
        /* GPIO_INPUT + DT 의 GPIO_PULL_UP 적용 (인터럽트는 안 검). */
        rc = gpio_pin_configure_dt(&s_pin_sw, GPIO_INPUT);
        if (rc != 0) {
            LOG_ERR("ROT_SW configure failed: %d", rc);
            return rc;
        }
        k_work_init_delayable(&s_sw_poll, rotary_sw_poll_fn);
        k_work_reschedule(&s_sw_poll, K_MSEC(ROT_SW_POLL_MS));
        printf("ROT: SW poll start GPIO%u logical=%d raw=%d\n",
               s_pin_sw.pin, gpio_pin_get_dt(&s_pin_sw),
               gpio_pin_get_raw(s_pin_sw.port, s_pin_sw.pin));
    }

    LOG_INF("EC16 ready: ROT_A=GPIO%u ROT_B=GPIO%u SW=GPIO%u init_state=%u",
            s_pin_a.pin, s_pin_b.pin, s_pin_sw.pin, s_state);
    return 0;
}

int32_t rotary_enc_get_count(void)
{
    return (int32_t)atomic_get(&s_count);
}

void rotary_enc_reset_count(void)
{
    atomic_set(&s_count, 0);
}

int32_t rotary_enc_get_sw_count(void)
{
    return (int32_t)atomic_get(&s_sw_count);
}

bool rotary_enc_sw_pressed(void)
{
    /* 현재 눌려있는지 (active-low → pressed=라인 LOW=logical 1) */
    return gpio_pin_get_dt(&s_pin_sw) == 1;
}
