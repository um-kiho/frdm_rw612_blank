/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE Matter hub stub GATT service (app/.matter 연동 전단).
 *
 * Service UUID base : 5a637562-eXXX-4000-8000-000000000001
 *   e900 : service
 *   e901 : cmd  (write, opcode byte + payload)
 *   e902 : status (read + notify, matter_hub_pack_status 레이아웃)
 *
 * Opcodes:
 *   0x01 FACTORY_RESET     Matter NVM 초기화 (Wi-Fi provisioning 유지).
 *                         payload 없음.
 *   0x02 WINDOW_OPEN      커미셔닝 윈도우 시작 (RAM 전용).
 *                         payload: 선택 u16 초( little-endian ).
 *                               없거나 2바 미만이면 기본 시간 사용.
 *   0x03 WINDOW_CLOSE     윈도우 즉시 종료.
 *   0x04 DEV_SIM_FABRIC   개발용: 패브릭 1개 시뮬 + COMMISSIONED 저장.
 */

#ifndef APP_BLE_SVC_MATTER_H
#define APP_BLE_SVC_MATTER_H

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MATTER_OP_FACTORY_RESET     0x01u
#define APP_MATTER_OP_WINDOW_OPEN       0x02u
#define APP_MATTER_OP_WINDOW_CLOSE      0x03u
#define APP_MATTER_OP_DEV_SIM_FABRIC    0x04u

int  app_matter_svc_init(void);
void app_matter_svc_publish_status(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_SVC_MATTER_H */
