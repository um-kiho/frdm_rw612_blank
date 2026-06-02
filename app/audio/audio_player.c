/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 오디오 플레이어 — ES8388 I2C 제어 전용
 *
 * RW612 I2C (FC2: GPIO16=SDA, GPIO17=SCL) → ES8388 볼륨/뮤트 제어
 * 오디오 데이터 재생은 ESP32-A1S가 담당 (RW612 I2S 미사용)
 *
 * I2C init은 k_work로 defer — 호출 스레드(bt_ready_cb) block 방지
 */

#include "audio_player.h"
#include "es8388.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(audio_player, LOG_LEVEL_INF);

static audio_player_state_t  s_state;
static audio_state_cb_t      s_state_cb;
static void                 *s_state_cb_arg;
static volatile bool         s_inited;
static uint8_t               s_vol;

static struct k_work         s_init_work;

static void set_state(audio_player_state_t st)
{
    s_state = st;
    if (s_state_cb != NULL) {
        s_state_cb(st, s_state_cb_arg);
    }
}

static void es8388_init_work_fn(struct k_work *w)
{
    ARG_UNUSED(w);

    es8388_config_t es_cfg = {
        .i2c_addr = ES8388_I2C_ADDR_LOW,
        .out_vol  = s_vol,
    };
    int rc = es8388_init(&es_cfg);
    if (rc == 0) {
        s_inited = true;
        set_state(AUDIO_STATE_READY);
        LOG_INF("ES8388 ready (addr=0x10) vol=0x%02X", s_vol);
    } else {
        set_state(AUDIO_STATE_ERROR);
        LOG_WRN("ES8388 not found (rc=%d) — volume control disabled", rc);
    }
}

int audio_player_init(const audio_player_config_t *cfg)
{
    if (cfg == NULL) {
        return -EINVAL;
    }
    s_state_cb     = cfg->state_cb;
    s_state_cb_arg = cfg->user_arg;
    s_vol          = cfg->es8388_vol;
    s_inited       = false;

    k_work_init(&s_init_work, es8388_init_work_fn);
    k_work_submit(&s_init_work);
    return 0;
}

int audio_player_start(void)  { return 0; }

int audio_player_pause(void)
{
    if (!s_inited) { return -EAGAIN; }
    es8388_set_mute(true);
    set_state(AUDIO_STATE_PAUSED);
    return 0;
}

int audio_player_resume(void)
{
    if (!s_inited) { return -EAGAIN; }
    es8388_set_mute(false);
    set_state(AUDIO_STATE_PLAYING);
    return 0;
}

int audio_player_set_volume(uint8_t vol)
{
    if (!s_inited) { return -EAGAIN; }
    s_vol = vol;
    return es8388_set_volume(vol);
}

int audio_player_write_pcm(const int16_t *buf, size_t len)
{
    ARG_UNUSED(buf);
    ARG_UNUSED(len);
    return -ENOTSUP;
}

audio_player_state_t audio_player_get_state(void)
{
    return s_state;
}
