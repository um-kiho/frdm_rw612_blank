/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Matter hub integration facade (CSA Matter/CHIP 전 단계 스텁).
 *
 * - app/.matter/matter_prefs*      : 패브릭 전 페이즈에서도 유지 가능한 상태·플래그 NVM
 * - matter_device_info             : VID/PID/버전 문자열(BLE/UI 연동 전단)
 *
 * 향후 NXP CHIP 스택 포팅 시 이 파일 안의 스텝 API 를 CHIP DeviceLayer 초기화로
 * 교체하면 됩니다.
 */

#ifndef APP_MATTER_HUB_CORE_H
#define APP_MATTER_HUB_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "matter_device_info.h"
#include "matter_prefs.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_MATTER_DEVICE_NAME
#define APP_MATTER_DEVICE_NAME "ZCUBE-matter-hub"
#endif

#ifndef APP_MATTER_DEFAULT_COMMISSION_SECONDS
#define APP_MATTER_DEFAULT_COMMISSION_SECONDS 180u
#endif

int  matter_hub_init(void);

/* app_main 의 1Hz tick 에서 호출 — 커미셔닝 윈도우 타임아웃 등 */
void matter_hub_on_tick(uint32_t uptime_s);

void matter_hub_get_descriptor(matter_device_descriptor_t *out);

void matter_hub_get_prefs_view(matter_prefs_payload_t *out);

/* 커미셔닝 윈도우 (CHIP 연동 전: 타이머·NVM 상태만 갱신) */
int  matter_hub_commission_window_open(uint32_t duration_s, uint32_t uptime_s);
int  matter_hub_commission_window_close(void);

/* Matter 관련 NVM 만 초기화 (Wi-Fi appcfg 는 유지) */
int  matter_hub_factory_reset(void);

/* 개발·회귀용: 패브릭 1개 추가된 것으로 시뮬레이션 */
int  matter_hub_dev_simulate_fabric_commissioned(void);

uint32_t matter_hub_commission_window_remaining_s(uint32_t uptime_s);

/* matter_hub_on_tick() 이 마지막으로 기록한 업타임 — BLE CMD 가 tick 전에
 * 도착하면 0 일 수 있음. */
uint32_t matter_hub_cached_uptime_s(void);

/* Packed status for BLE NOTIFY (little-endian multi-byte fields).
 * Bytes:
 *    [0]    effective commissioning state (matter_commissioning_state_t)
 *    [1]    fabric_count
 * [2..3]  window remaining seconds (LE); 0 if window inactive
 * [4..5]  vendor_id (LE)
 * [6..7]  product_id (LE)
 * [8..9]  hardware_version (LE)
 * [10..11] firmware_version / software_version (LE)
 * [12..15] reserved (0)
 *
 * 윈도우 잔여 시간은 matter_hub_on_tick() 이 갱신하는 내부 업타임 캐시를 사용합니다.
 */
#ifndef MATTER_HUB_STATUS_PACKED_LEN
#define MATTER_HUB_STATUS_PACKED_LEN  16u
#endif

void matter_hub_pack_status(uint8_t buf[MATTER_HUB_STATUS_PACKED_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* APP_MATTER_HUB_CORE_H */
