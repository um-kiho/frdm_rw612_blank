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

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(rotary_enc, LOG_LEVEL_INF);

/* DT alias에서 GPIO 스펙 추출 */
#define ROT_A_NODE  DT_ALIAS(rot_a)
#define ROT_B_NODE  DT_ALIAS(rot_b)

static const struct gpio_dt_spec s_pin_a =
    GPIO_DT_SPEC_GET(ROT_A_NODE, gpios);
static const struct gpio_dt_spec s_pin_b =
    GPIO_DT_SPEC_GET(ROT_B_NODE, gpios);

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

    LOG_INF("EC16 ready: ROT_A=GPIO%u ROT_B=GPIO%u init_state=%u",
            s_pin_a.pin, s_pin_b.pin, s_state);
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
