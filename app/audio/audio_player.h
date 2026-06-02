/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 오디오 플레이어 — BT A2DP 수신 → SBC 디코드 → I2S → ES8388
 *
 * 파이프라인:
 *   Phone (A2DP Source)
 *     ↓ BT Classic (BR/EDR)
 *   audio_player (A2DP Sink)
 *     ↓ SBC decode
 *   I2S TX (FC1: GPIO8=BCLK, GPIO9=LRCLK, GPIO10=TXDATA)
 *     ↓
 *   ES8388 DAC → LOUT1/ROUT1 → Speaker/Headphone
 *
 * 필요 prj.conf 추가:
 *   CONFIG_BT_CLASSIC=y
 *   CONFIG_BT_CONN=y
 *   CONFIG_BT_A2DP=y
 *   CONFIG_BT_AVDTP=y
 *   CONFIG_BT_SBC_DEC=y
 *   CONFIG_I2S=y
 */

#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum audio_player_state {
    AUDIO_STATE_IDLE    = 0,
    AUDIO_STATE_READY,       /* I2S + ES8388 초기화 완료 */
    AUDIO_STATE_CONNECTED,   /* BT A2DP 연결됨 */
    AUDIO_STATE_PLAYING,     /* 재생 중 */
    AUDIO_STATE_PAUSED,
    AUDIO_STATE_ERROR,
} audio_player_state_t;

typedef void (*audio_state_cb_t)(audio_player_state_t state, void *user_arg);

typedef struct audio_player_config {
    uint32_t         sample_rate;   /* 44100 또는 48000 */
    uint8_t          es8388_vol;    /* 출력 볼륨 0x00(최대)~0x21(-33dB) */
    audio_state_cb_t state_cb;
    void            *user_arg;
} audio_player_config_t;

/** I2S + ES8388 초기화, BT A2DP Sink 등록
 *  @return 0 on success */
int  audio_player_init  (const audio_player_config_t *cfg);

/** BT A2DP 연결 대기 시작 (Discoverable) */
int  audio_player_start (void);

/** 재생 일시 정지 / 재개 */
int  audio_player_pause (void);
int  audio_player_resume(void);

/** 볼륨 조절 (ES8388 LOUT1/ROUT1) */
int  audio_player_set_volume(uint8_t vol);

/** PCM 데이터 직접 재생 (BT 없이 I2S 테스트용)
 *  buf: 16-bit stereo interleaved PCM, len: bytes */
int  audio_player_write_pcm(const int16_t *buf, size_t len);

audio_player_state_t audio_player_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PLAYER_H */
