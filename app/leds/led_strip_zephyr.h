/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Unified Zephyr LED strip driver — SK6812 / APA102 선택적 사용.
 *
 * SK6812 RGBW : 단선 NRZ, GPIO bit-bang (GPIO46 = HSGPIO1[14])
 *               irq_lock()으로 IRQ 차단하여 타이밍 지터 방지
 * APA102      : CLK+DATA GPIO bit-bang (GPIO47 DATA, GPIO48 CLK = HSGPIO1[15/16])
 *               타이밍 제약 없음 — LCDIC 비활성화 핀 재활용
 *
 * 빌드 선택: CONFIG_APP_LED_APA102=y → APA102, 기본값 SK6812
 *
 * 공통 픽셀 형식: RGBW (APA102는 W 채널 무시)
 * 글로벌 밝기 조절: 0(꺼짐)~255(최대)
 */

#ifndef LED_STRIP_ZEPHYR_H
#define LED_STRIP_ZEPHYR_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 최대 지원 LED 수 (픽셀 버퍼 정적 할당) */
#ifndef LED_STRIP_MAX_COUNT
#define LED_STRIP_MAX_COUNT  64u
#endif

typedef struct rgbw_color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t w;  /* APA102는 무시됨 */
} rgbw_color_t;

#define RGBW(R,G,B,W)  ((rgbw_color_t){(R),(G),(B),(W)})
#define RGB(R,G,B)     ((rgbw_color_t){(R),(G),(B),0u})
#define COLOR_OFF      ((rgbw_color_t){0u,0u,0u,0u})

typedef enum {
    LED_STRIP_SK6812 = 0,   /* 단선 NRZ GPIO bit-bang */
    LED_STRIP_APA102 = 1,   /* CLK+DATA GPIO bit-bang */
} led_strip_type_t;

typedef struct led_strip_config {
    led_strip_type_t    type;
    uint16_t            count;      /* LED 수, LED_STRIP_MAX_COUNT 이하 */
    uint8_t             brightness; /* 초기 밝기 0-255 */

    /* 공통: gpio_dev = 두 타입 모두 HSGPIO1 사용 */
    const struct device *gpio_dev;
    gpio_pin_t           gpio_pin;      /* SK6812: DATA / APA102: DATA */
    gpio_pin_t           gpio_pin_clk;  /* APA102 전용 CLK pin (SK6812는 무시) */
} led_strip_config_t;

int      led_strip_init          (const led_strip_config_t *cfg);

/* 개별 픽셀 설정 (refresh() 전까지 버퍼에만 저장) */
int      led_strip_set_pixel     (uint16_t idx, rgbw_color_t color);

/* 전체 스트립을 동일 색으로 채움 */
int      led_strip_fill          (rgbw_color_t color);

/* 전체 OFF 후 즉시 refresh */
int      led_strip_clear         (void);

/* 버퍼 → 하드웨어 출력 */
int      led_strip_refresh       (void);

/* 글로벌 밝기 (encode 시 적용, 픽셀 버퍼 보존) */
void     led_strip_set_brightness(uint8_t br);
uint8_t  led_strip_get_brightness(void);

uint16_t led_strip_count         (void);

#ifdef __cplusplus
}
#endif

#endif /* LED_STRIP_ZEPHYR_H */
