/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Zephyr LED task backend — led_task.h API를 led_strip_zephyr 위에 구현.
 *
 * 빌드 선택: CONFIG_APP_LED_APA102=y → APA102 (GPIO47 DATA, GPIO48 CLK)
 *            기본값  → SK6812 RGBW (GPIO46)
 *
 * 애니메이션 tick: 20 ms 주기 k_timer → system workq
 *   LED_PAT_OFF       : 소등
 *   LED_PAT_SOLID     : 단색 유지
 *   LED_PAT_BREATHING : 삼각파 엔벨로프 (4초 주기)
 *   LED_PAT_RAINBOW   : HSV 색상 회전
 *   LED_PAT_STROBE    : 100 ms on / 100 ms off
 */

#include "led_task.h"
#include "led_strip_zephyr.h"

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_led_task, LOG_LEVEL_INF);

/* ── 하드웨어 바인딩 ────────────────────────────────────────────────── */
/* SK6812 : GPIO46 = HSGPIO1[14]  DATA (단선 NRZ) */
/* APA102 : GPIO47 = HSGPIO1[15]  DATA
 *          GPIO48 = HSGPIO1[16]  CLK  (LCDIC 비활성화 핀 재활용) */
#define LED_GPIO_NODE    DT_NODELABEL(hsgpio1)
#define SK6812_DATA_PIN  14u   /* GPIO46 */
#define APA102_DATA_PIN  15u   /* GPIO47 */
#define APA102_CLK_PIN   16u   /* GPIO48 */

/* ── 애니메이션 상수 ─────────────────────────────────────────────────  */
#define TICK_MS          20u
#define BREATHING_TICKS  200u   /* 4초 주기 (200 × 20 ms) */
#define STROBE_TICKS     5u     /* 100 ms (5 × 20 ms) */

/* ── 모듈 상태 ─────────────────────────────────────────────────────── */
static bool              s_started;
static led_pattern_t     s_pattern  = LED_PAT_OFF;
static sk6812_color_t    s_color    = SK6812_OFF;
static uint8_t           s_brightness = 0xFFu;
static sleep_light_preset_t s_sleep_preset = SLEEP_LIGHT_OFF;
static uint8_t           s_sleep_brightness_pct;

static struct k_timer    s_timer;
static struct k_work     s_tick_work;
static uint32_t          s_tick;

/* ── HSV → RGB 변환 (H:0-360, S/V:0-255) ──────────────────────────── */
static rgbw_color_t hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
    if (s == 0u) {
        return RGBW(v, v, v, 0u);
    }

    uint16_t region  = h / 60u;
    uint16_t remain  = h % 60u;
    uint8_t  p = (uint8_t)((uint16_t)v * (255u - s) / 255u);
    uint8_t  q = (uint8_t)((uint16_t)v * (255u - ((uint16_t)s * remain) / 60u) / 255u);
    uint8_t  t = (uint8_t)((uint16_t)v * (255u - ((uint16_t)s * (60u - remain)) / 60u) / 255u);

    switch (region) {
    case 0:  return RGBW(v, t, p, 0u);
    case 1:  return RGBW(q, v, p, 0u);
    case 2:  return RGBW(p, v, t, 0u);
    case 3:  return RGBW(p, q, v, 0u);
    case 4:  return RGBW(t, p, v, 0u);
    default: return RGBW(v, p, q, 0u);
    }
}

/* ── sk6812_color → rgbw_color 변환 ────────────────────────────────── */
static inline rgbw_color_t to_rgbw(sk6812_color_t c)
{
    return RGBW(c.r, c.g, c.b, c.w);
}

/* ── 애니메이션 tick ───────────────────────────────────────────────── */
static void tick_fn(struct k_work *w)
{
    ARG_UNUSED(w);

    uint16_t count = led_strip_count();

    if (count == 0u) {
        return;
    }

    switch (s_pattern) {
    case LED_PAT_OFF:
        led_strip_clear();
        return;

    case LED_PAT_SOLID:
        led_strip_fill(to_rgbw(s_color));
        break;

    case LED_PAT_BREATHING: {
        /* 삼각파: 0→255→0, BREATHING_TICKS 주기 */
        uint32_t phase = s_tick % BREATHING_TICKS;
        uint8_t  env;

        if (phase < BREATHING_TICKS / 2u) {
            env = (uint8_t)(phase * 255u / (BREATHING_TICKS / 2u));
        } else {
            env = (uint8_t)((BREATHING_TICKS - phase) * 255u / (BREATHING_TICKS / 2u));
        }

        rgbw_color_t c = to_rgbw(s_color);

        c.r = (uint8_t)((uint16_t)c.r * env / 255u);
        c.g = (uint8_t)((uint16_t)c.g * env / 255u);
        c.b = (uint8_t)((uint16_t)c.b * env / 255u);
        c.w = (uint8_t)((uint16_t)c.w * env / 255u);
        led_strip_fill(c);
        break;
    }

    case LED_PAT_RAINBOW: {
        uint16_t hue_base = (uint16_t)((s_tick * 3u) % 360u);

        for (uint16_t i = 0; i < count; i++) {
            uint16_t hue = (hue_base + i * 360u / count) % 360u;
            rgbw_color_t c = hsv_to_rgb(hue, 255u, 255u);

            c.w = s_color.w;
            led_strip_set_pixel(i, c);
        }
        break;
    }

    case LED_PAT_STROBE: {
        bool on = (s_tick / STROBE_TICKS) % 2u == 0u;

        led_strip_fill(on ? to_rgbw(s_color) : COLOR_OFF);
        break;
    }

    default:
        break;
    }

    led_strip_refresh();
    s_tick++;
}

static void timer_expiry(struct k_timer *t)
{
    ARG_UNUSED(t);
    k_work_submit(&s_tick_work);
}

/* ── 공개 API ───────────────────────────────────────────────────────── */

int led_task_start(void)
{
    if (s_started) {
        return 0;
    }

#if defined(CONFIG_APP_LED_APA102)
    led_strip_config_t cfg = {
        .type         = LED_STRIP_APA102,
        .count        = APP_LED_COUNT,
        .brightness   = 0xFFu,
        .gpio_dev     = DEVICE_DT_GET(LED_GPIO_NODE),
        .gpio_pin     = APA102_DATA_PIN,
        .gpio_pin_clk = APA102_CLK_PIN,
    };
#else
    led_strip_config_t cfg = {
        .type         = LED_STRIP_SK6812,
        .count        = APP_LED_COUNT,
        .brightness   = 0xFFu,
        .gpio_dev     = DEVICE_DT_GET(LED_GPIO_NODE),
        .gpio_pin     = SK6812_DATA_PIN,
    };
#endif

    int rc = led_strip_init(&cfg);

    if (rc != 0) {
        LOG_ERR("led_strip_init failed: %d", rc);
        return rc;
    }

    k_work_init(&s_tick_work, tick_fn);
    k_timer_init(&s_timer, timer_expiry, NULL);
    k_timer_start(&s_timer, K_MSEC(TICK_MS), K_MSEC(TICK_MS));

    s_started = true;
#if defined(CONFIG_APP_LED_APA102)
    LOG_INF("LED task started: APA102 %u LEDs GPIO47(DATA)/GPIO48(CLK) tick=%ums",
            APP_LED_COUNT, TICK_MS);
#else
    LOG_INF("LED task started: SK6812 %u LEDs GPIO46 tick=%ums",
            APP_LED_COUNT, TICK_MS);
#endif
    return 0;
}

void led_task_set_pattern(led_pattern_t p, sk6812_color_t base)
{
    s_pattern = p;
    s_color   = base;
    s_tick    = 0u;
}

void led_task_set_color(sk6812_color_t base)
{
    s_color = base;
}

void led_task_set_brightness(uint8_t br)
{
    s_brightness = br;
    led_strip_set_brightness(br);
}

int led_task_set_pixel(uint16_t idx, sk6812_color_t c)
{
    return led_strip_set_pixel(idx, to_rgbw(c));
}

void led_task_set_sleep_light(sleep_light_preset_t preset, uint8_t brightness_pct)
{
    s_sleep_preset          = preset;
    s_sleep_brightness_pct  = brightness_pct;
    s_pattern               = LED_PAT_SOLID;
    s_color                 = sleep_light_color(preset, brightness_pct);
}

void led_task_get_sleep_light(sleep_light_preset_t *preset_out,
                              uint8_t *brightness_pct_out)
{
    if (preset_out != NULL) {
        *preset_out = s_sleep_preset;
    }
    if (brightness_pct_out != NULL) {
        *brightness_pct_out = s_sleep_brightness_pct;
    }
}

led_pattern_t led_task_get_pattern(void)
{
    return s_pattern;
}

sk6812_color_t led_task_get_color(void)
{
    return s_color;
}

/* ── sk6812.h 호환 API (svc_led 등에서 직접 사용) ──────────────────── */

int sk6812_init(void)
{
    return led_task_start();
}

uint16_t sk6812_count(void)
{
    return led_strip_count();
}

int sk6812_set_pixel(uint16_t idx, sk6812_color_t c)
{
    return led_strip_set_pixel(idx, to_rgbw(c));
}

int sk6812_fill(sk6812_color_t c)
{
    return led_strip_fill(to_rgbw(c));
}

int sk6812_fill_scaled(sk6812_color_t c, uint8_t brightness_pct)
{
    rgbw_color_t sc = {
        .r = (uint8_t)((uint16_t)c.r * brightness_pct / 100u),
        .g = (uint8_t)((uint16_t)c.g * brightness_pct / 100u),
        .b = (uint8_t)((uint16_t)c.b * brightness_pct / 100u),
        .w = (uint8_t)((uint16_t)c.w * brightness_pct / 100u),
    };
    return led_strip_fill(sc);
}

int sk6812_set_pixel_scaled(uint16_t idx, sk6812_color_t c, uint8_t brightness_pct)
{
    rgbw_color_t sc = {
        .r = (uint8_t)((uint16_t)c.r * brightness_pct / 100u),
        .g = (uint8_t)((uint16_t)c.g * brightness_pct / 100u),
        .b = (uint8_t)((uint16_t)c.b * brightness_pct / 100u),
        .w = (uint8_t)((uint16_t)c.w * brightness_pct / 100u),
    };
    return led_strip_set_pixel(idx, sc);
}

int sk6812_clear(void)
{
    return led_strip_clear();
}

void sk6812_set_brightness(uint8_t scale)
{
    led_strip_set_brightness(scale);
}

uint8_t sk6812_get_brightness(void)
{
    return led_strip_get_brightness();
}

int sk6812_refresh(void)
{
    return led_strip_refresh();
}
