/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ES8388 코덱 드라이버 — i2c_bus 래퍼 사용 (FC2, mutex 보호)
 *
 * 초기화 순서: ESP-ADF es8388_codec.c 기반
 *   1. Chip power on, slave mode
 *   2. ADC power down (재생 전용)
 *   3. DAC power on + LOUT1/ROUT1 enable
 *   4. I2S format 16-bit, slave
 *   5. DAC → Mixer → Output 라우팅
 */

#include "es8388.h"
#include "i2c_bus.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(es8388, LOG_LEVEL_INF);

static uint8_t s_addr;

/* ── 저수준 I2C ─────────────────────────────────────────────────────── */

int es8388_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    return i2c_bus_write(s_addr, buf, sizeof(buf));
}

int es8388_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_bus_write_read(s_addr, &reg, 1, val, 1);
}

/* ── 초기화 ─────────────────────────────────────────────────────────── */

int es8388_init(const es8388_config_t *cfg)
{
    if (cfg == NULL) {
        return -EINVAL;
    }

    int rc = i2c_bus_init();
    if (rc != 0) {
        LOG_ERR("i2c_bus_init failed: %d", rc);
        return rc;
    }

    s_addr = cfg->i2c_addr;

#define WR(reg, val) do { rc = es8388_write_reg((reg), (val)); if (rc) { LOG_ERR("reg 0x%02X write failed: %d", (reg), rc); return rc; } } while (0)

    /* 1. 소프트 리셋 */
    WR(ES8388_CONTROL1,    0x80u);
    k_msleep(10);
    WR(ES8388_CONTROL1,    0x06u);

    /* 2. 전원 관리 */
    WR(ES8388_CONTROL2,    0x1Cu);
    WR(ES8388_CHIPPOWER,   0x00u);
    WR(ES8388_ADCPOWER,    0xFFu);   /* ADC power off (재생 전용) */

    /* 3. Slave 모드: MCU(flexcomm1)가 BCLK/LRCLK 공급, ES8388은 수신 */
    WR(ES8388_MASTERMODE,  0x00u);

    /* 4. DAC: I2S 포맷, 16-bit */
    WR(ES8388_DACCONTROL1, 0x18u);
    WR(ES8388_DACCONTROL2, 0x02u);   /* DEM off */

    /* 5. DAC 볼륨 0dB */
    WR(ES8388_DACVOLL,     0x00u);
    WR(ES8388_DACVOLR,     0x00u);

    /* 6. Mixer: DAC → LOUT1/ROUT1 */
    WR(ES8388_DACCONTROL16, 0x00u);
    WR(ES8388_DACCONTROL17, 0x90u);
    WR(ES8388_DACCONTROL20, 0x90u);
    WR(ES8388_DACCONTROL21, 0xA0u);

    /* 7. 출력 볼륨 */
    WR(ES8388_LOUT1VOL, cfg->out_vol);
    WR(ES8388_ROUT1VOL, cfg->out_vol);

    /* 8. DAC power on */
    WR(ES8388_DACPOWER, 0x3Cu);

    /* 9. 언뮤트 */
    WR(ES8388_DACCONTROL3, 0x00u);

#undef WR

    LOG_INF("ES8388 ready addr=0x%02X vol=0x%02X", s_addr, cfg->out_vol);
    return 0;
}

/* ── 볼륨 / 뮤트 ────────────────────────────────────────────────────── */

int es8388_set_volume(uint8_t vol)
{
    int rc = es8388_write_reg(ES8388_LOUT1VOL, vol);

    if (rc == 0) {
        rc = es8388_write_reg(ES8388_ROUT1VOL, vol);
    }
    return rc;
}

int es8388_set_mute(bool mute)
{
    uint8_t val;
    int rc = es8388_read_reg(ES8388_DACCONTROL3, &val);

    if (rc != 0) {
        return rc;
    }

    val = mute ? (val | 0x04u) : (val & ~0x04u);
    return es8388_write_reg(ES8388_DACCONTROL3, val);
}
