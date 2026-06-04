/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * EC16 로터리 인코더 — Zephyr GPIO 인터럽트 + 쿼드러처 룩업테이블
 *
 * 핀 할당 (frdm_rw612.overlay):
 *   ROT_A → GPIO4  (HSGPIO0[4])  alias: rot-a
 *   ROT_B → GPIO5  (HSGPIO0[5])  alias: rot-b
 *
 * CW  → count 증가(+)
 * CCW → count 감소(-)
 */

#ifndef ROTARY_ENC_H
#define ROTARY_ENC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** GPIO 핀 설정 및 ANY_EDGE 인터럽트 등록.
 *  @return 0 on success, negative errno on failure */
int rotary_enc_init(void);

/** ISR에서 누적된 스텝 (CW: +, CCW: -) */
int32_t rotary_enc_get_count(void);

/** 카운터를 0으로 리셋 */
void rotary_enc_reset_count(void);

/** SW(푸시 버튼) 누적 누름 횟수 (디바운스 적용) */
int32_t rotary_enc_get_sw_count(void);

/** SW 가 지금 눌려있는지 (true=눌림) */
bool rotary_enc_sw_pressed(void);

#ifdef __cplusplus
}
#endif

#endif /* ROTARY_ENC_H */
