/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ES8388 오디오 코덱 드라이버 (Everest Semiconductor)
 * ESP32-A1S / AI-Thinker Audio Kit 탑재 코덱
 *
 * 제어 인터페이스: I2C  (기존 FC2 버스, GPIO16/17)
 * 오디오 인터페이스: I2S (FC1, GPIO7=BCLK / GPIO8=LRCLK / GPIO9=TXDATA)
 *
 * I2C 주소: 0x10 (CSADDR=GND) / 0x11 (CSADDR=VDD)
 */

#ifndef ES8388_H
#define ES8388_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── I2C 주소 ────────────────────────────────────────────────────────── */
#define ES8388_I2C_ADDR_LOW   0x10u   /* CSADDR=GND (기본) */
#define ES8388_I2C_ADDR_HIGH  0x11u   /* CSADDR=VDD */

/* ── 레지스터 맵 ─────────────────────────────────────────────────────── */
#define ES8388_CONTROL1       0x00u
#define ES8388_CONTROL2       0x01u
#define ES8388_CHIPPOWER      0x02u
#define ES8388_ADCPOWER       0x03u
#define ES8388_DACPOWER       0x04u
#define ES8388_CHIPLOPWR1     0x05u
#define ES8388_CHIPLOPWR2     0x06u
#define ES8388_ANAVOLMANAGE   0x07u
#define ES8388_MASTERMODE     0x08u
#define ES8388_ADCCONTROL1    0x09u
#define ES8388_ADCCONTROL2    0x0Au
#define ES8388_ADCCONTROL3    0x0Bu
#define ES8388_ADCCONTROL4    0x0Cu
#define ES8388_ADCCONTROL5    0x0Du
#define ES8388_ADCCONTROL6    0x0Eu
#define ES8388_ADCCONTROL7    0x0Fu
#define ES8388_ADCCONTROL8    0x10u
#define ES8388_ADCCONTROL9    0x11u
#define ES8388_ADCCONTROL10   0x12u
#define ES8388_ADCCONTROL11   0x13u
#define ES8388_ADCCONTROL12   0x14u
#define ES8388_ADCCONTROL13   0x15u
#define ES8388_ADCCONTROL14   0x16u
#define ES8388_DACCONTROL1    0x17u
#define ES8388_DACCONTROL2    0x18u
#define ES8388_DACCONTROL3    0x19u
#define ES8388_DACVOLL        0x1Au
#define ES8388_DACVOLR        0x1Bu
#define ES8388_DACCONTROL6    0x1Cu
#define ES8388_DACCONTROL7    0x1Du
#define ES8388_DACCONTROL8    0x1Eu
#define ES8388_DACCONTROL9    0x1Fu
#define ES8388_DACCONTROL10   0x20u
#define ES8388_DACCONTROL11   0x21u
#define ES8388_DACCONTROL12   0x22u
#define ES8388_DACCONTROL13   0x23u
#define ES8388_DACCONTROL14   0x24u
#define ES8388_DACCONTROL15   0x25u
#define ES8388_DACCONTROL16   0x26u
#define ES8388_DACCONTROL17   0x27u
#define ES8388_DACCONTROL18   0x28u
#define ES8388_DACCONTROL19   0x29u
#define ES8388_DACCONTROL20   0x2Au
#define ES8388_DACCONTROL21   0x2Bu
#define ES8388_DACCONTROL22   0x2Cu
#define ES8388_DACCONTROL23   0x2Du
#define ES8388_LOUT1VOL       0x2Eu
#define ES8388_ROUT1VOL       0x2Fu
#define ES8388_LOUT2VOL       0x30u
#define ES8388_ROUT2VOL       0x31u
#define ES8388_DACCONTROL30   0x32u

/* ── 설정 구조체 ─────────────────────────────────────────────────────── */
typedef struct es8388_config {
    uint8_t i2c_addr;   /* ES8388_I2C_ADDR_LOW or HIGH */
    uint8_t out_vol;    /* 출력 볼륨 0x00(최대)~0x21(-33dBFS) */
} es8388_config_t;

/** 코덱 초기화 (DAC 재생 전용 모드, I2S 슬레이브)
 *  @return 0 on success, negative errno on failure */
int es8388_init(const es8388_config_t *cfg);

/** 출력 볼륨 설정 (LOUT1/ROUT1): 0x00=최대, 0x21=-33dB, 0x21 이상=뮤트 */
int es8388_set_volume(uint8_t vol);

/** DAC 뮤트/언뮤트 */
int es8388_set_mute(bool mute);

/** 레지스터 직접 읽기/쓰기 (디버그/튜닝용) */
int es8388_write_reg(uint8_t reg, uint8_t val);
int es8388_read_reg(uint8_t reg, uint8_t *val);

#ifdef __cplusplus
}
#endif

#endif /* ES8388_H */
