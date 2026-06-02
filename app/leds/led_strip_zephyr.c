/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Zephyr LED strip driver — SK6812 / APA102 GPIO bit-bang
 *
 * ── SK6812 타이밍 (단선 NRZ, 800 kHz) ──────────────────────────────────
 *   Bit "0": T0H = 300 ns HIGH, T0L = 900 ns LOW
 *   Bit "1": T1H = 600 ns HIGH, T1L = 600 ns LOW
 *   Reset  : 80 µs LOW
 *   Byte order per LED: G R B W  (MSB first)
 *
 * ── APA102 GPIO bit-bang ────────────────────────────────────────────────
 *   Start frame : 4 × 0x00
 *   LED frame   : 0xE0|(br5 & 0x1F), Blue, Green, Red
 *   End frame   : ceil(count/2 + 1) × 0xFF
 *   CLK idle low; DATA 셋업 후 CLK 상승 에지에서 래치 (SPI Mode 0)
 *   타이밍 제약 없음 — 임의 속도로 동작
 *
 * ── Cycle-counter 기반 ns 딜레이 (SK6812 전용) ──────────────────────────
 *   CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC 컴파일 타임 상수를 이용해
 *   이식성 있는 ns 딜레이를 구현한다.
 */

#include "led_strip_zephyr.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_strip, LOG_LEVEL_INF);

/* ── 컴파일 타임 ns → 사이클 변환 ────────────────────────────────────── */
#define NS_TO_CYC(ns) \
    ((uint32_t)((uint64_t)(ns) * CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000000ULL))

#define SK6812_T0H_CYC  NS_TO_CYC(300u)
#define SK6812_T0L_CYC  NS_TO_CYC(900u)
#define SK6812_T1H_CYC  NS_TO_CYC(600u)
#define SK6812_T1L_CYC  NS_TO_CYC(600u)
#define SK6812_RST_US   80u

/* ── 모듈 상태 ─────────────────────────────────────────────────────────*/
static led_strip_config_t s_cfg;
static rgbw_color_t       s_pixels[LED_STRIP_MAX_COUNT];
static uint8_t            s_brightness = 255u;
static bool               s_inited;

/* ────────────────────────────────────────────────────────────────────── *
 * 내부 유틸리티
 * ────────────────────────────────────────────────────────────────────── */

static inline uint8_t apply_brightness(uint8_t ch)
{
    return (uint8_t)(((uint16_t)ch * s_brightness) >> 8);
}

/* ────────────────────────────────────────────────────────────────────── *
 * SK6812  —  단선 NRZ GPIO bit-bang
 * ────────────────────────────────────────────────────────────────────── */

static inline void wait_cycles(uint32_t cyc)
{
    uint32_t start = k_cycle_get_32();

    while ((k_cycle_get_32() - start) < cyc) {
        /* spin */
    }
}

static inline void sk6812_send_bit(bool one)
{
    uint32_t t_h = one ? SK6812_T1H_CYC : SK6812_T0H_CYC;
    uint32_t t_l = one ? SK6812_T1L_CYC : SK6812_T0L_CYC;

    gpio_pin_set_raw(s_cfg.gpio_dev, s_cfg.gpio_pin, 1);
    wait_cycles(t_h);
    gpio_pin_set_raw(s_cfg.gpio_dev, s_cfg.gpio_pin, 0);
    wait_cycles(t_l);
}

static inline void sk6812_send_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        sk6812_send_bit((byte >> i) & 0x01u);
    }
}

static int sk6812_refresh(void)
{
    unsigned int key = irq_lock();

    for (uint16_t i = 0; i < s_cfg.count; i++) {
        sk6812_send_byte(apply_brightness(s_pixels[i].g));
        sk6812_send_byte(apply_brightness(s_pixels[i].r));
        sk6812_send_byte(apply_brightness(s_pixels[i].b));
        sk6812_send_byte(apply_brightness(s_pixels[i].w));
    }

    irq_unlock(key);

    gpio_pin_set_raw(s_cfg.gpio_dev, s_cfg.gpio_pin, 0);
    k_busy_wait(SK6812_RST_US);

    return 0;
}

static int sk6812_init(void)
{
    if (!device_is_ready(s_cfg.gpio_dev)) {
        LOG_ERR("SK6812 GPIO device not ready");
        return -ENODEV;
    }

    int rc = gpio_pin_configure(s_cfg.gpio_dev, s_cfg.gpio_pin,
                                GPIO_OUTPUT_INACTIVE);

    if (rc != 0) {
        LOG_ERR("SK6812 GPIO configure failed: %d", rc);
        return rc;
    }

    LOG_INF("SK6812 init: %u LEDs DATA=HSGPIO1[%u]",
            s_cfg.count, s_cfg.gpio_pin);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────── *
 * APA102  —  CLK + DATA GPIO bit-bang (SPI Mode 0, 임의 속도)
 *
 * 핀 배선 (LCDIC 비활성화 핀 재활용):
 *   DATA = HSGPIO1[15] = GPIO47
 *   CLK  = HSGPIO1[16] = GPIO48
 * ────────────────────────────────────────────────────────────────────── */

#define APA102_START_BYTES   4u
#define APA102_END_BYTES(n)  ((n) / 2u + 2u)
#define APA102_BUF_SIZE(n)   (APA102_START_BYTES + (n) * 4u + APA102_END_BYTES(n))

static uint8_t s_apa102_buf[APA102_BUF_SIZE(LED_STRIP_MAX_COUNT)];

/* 1 바이트 전송: DATA 셋업 → CLK 상승 에지 → CLK 하강 에지 (MSB first) */
static inline void apa102_send_byte(uint8_t byte)
{
    for (int i = 7; i >= 0; i--) {
        gpio_pin_set_raw(s_cfg.gpio_dev, s_cfg.gpio_pin,
                         (byte >> i) & 0x01u);
        gpio_pin_set_raw(s_cfg.gpio_dev, s_cfg.gpio_pin_clk, 1);
        gpio_pin_set_raw(s_cfg.gpio_dev, s_cfg.gpio_pin_clk, 0);
    }
}

static int apa102_refresh(void)
{
    uint16_t n   = s_cfg.count;
    uint8_t  br5 = (uint8_t)(s_brightness >> 3);
    uint8_t *p   = s_apa102_buf;

    /* Start frame */
    memset(p, 0x00, APA102_START_BYTES);
    p += APA102_START_BYTES;

    /* LED frames: 0xE0|(br5), B, G, R */
    for (uint16_t i = 0; i < n; i++) {
        *p++ = 0xE0u | (br5 & 0x1Fu);
        *p++ = apply_brightness(s_pixels[i].b);
        *p++ = apply_brightness(s_pixels[i].g);
        *p++ = apply_brightness(s_pixels[i].r);
    }

    /* End frame */
    uint16_t end_bytes = (uint16_t)APA102_END_BYTES(n);

    memset(p, 0xFFu, end_bytes);

    /* 버퍼 전송 */
    uint16_t total = (uint16_t)(APA102_START_BYTES + n * 4u + end_bytes);

    for (uint16_t i = 0; i < total; i++) {
        apa102_send_byte(s_apa102_buf[i]);
    }

    return 0;
}

static int apa102_init(void)
{
    if (!device_is_ready(s_cfg.gpio_dev)) {
        LOG_ERR("APA102 GPIO device not ready");
        return -ENODEV;
    }

    int rc = gpio_pin_configure(s_cfg.gpio_dev, s_cfg.gpio_pin,
                                GPIO_OUTPUT_INACTIVE);  /* DATA */

    if (rc == 0) {
        rc = gpio_pin_configure(s_cfg.gpio_dev, s_cfg.gpio_pin_clk,
                                GPIO_OUTPUT_INACTIVE);  /* CLK idle low */
    }

    if (rc != 0) {
        LOG_ERR("APA102 GPIO configure failed: %d", rc);
        return rc;
    }

    LOG_INF("APA102 init: %u LEDs DATA=HSGPIO1[%u] CLK=HSGPIO1[%u]",
            s_cfg.count, s_cfg.gpio_pin, s_cfg.gpio_pin_clk);
    return 0;
}

/* ────────────────────────────────────────────────────────────────────── *
 * 공개 API
 * ────────────────────────────────────────────────────────────────────── */

int led_strip_init(const led_strip_config_t *cfg)
{
    if (cfg == NULL || cfg->count == 0u || cfg->count > LED_STRIP_MAX_COUNT) {
        return -EINVAL;
    }

    s_cfg        = *cfg;
    s_brightness = cfg->brightness;
    s_inited     = false;

    memset(s_pixels,     0, sizeof(s_pixels));
    memset(s_apa102_buf, 0, sizeof(s_apa102_buf));

    int rc = (cfg->type == LED_STRIP_SK6812) ? sk6812_init() : apa102_init();

    if (rc != 0) {
        return rc;
    }

    s_inited = true;
    (void)led_strip_clear();
    return 0;
}

int led_strip_set_pixel(uint16_t idx, rgbw_color_t color)
{
    if (!s_inited || idx >= s_cfg.count) {
        return -EINVAL;
    }

    s_pixels[idx] = color;
    return 0;
}

int led_strip_fill(rgbw_color_t color)
{
    if (!s_inited) {
        return -EAGAIN;
    }

    for (uint16_t i = 0; i < s_cfg.count; i++) {
        s_pixels[i] = color;
    }

    return 0;
}

int led_strip_clear(void)
{
    if (!s_inited) {
        return -EAGAIN;
    }

    memset(s_pixels, 0, s_cfg.count * sizeof(rgbw_color_t));
    return led_strip_refresh();
}

int led_strip_refresh(void)
{
    if (!s_inited) {
        return -EAGAIN;
    }

    return (s_cfg.type == LED_STRIP_SK6812) ? sk6812_refresh() : apa102_refresh();
}

void led_strip_set_brightness(uint8_t br)
{
    s_brightness = br;
}

uint8_t led_strip_get_brightness(void)
{
    return s_brightness;
}

uint16_t led_strip_count(void)
{
    return s_inited ? s_cfg.count : 0u;
}
