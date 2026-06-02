/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef APP_MAIN_H
#define APP_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Main application task entry point */
void app_main_task(void *arg);

/* 전체 스레드 스택 사용량 출력 (30초마다 자동 호출, 수동 호출 가능) */
void app_main_dump_stacks(void);

/* Stack size for main application task */
#define APP_MAIN_STACK_SIZE  1024

/* Priority for main application task */
#define APP_MAIN_PRIORITY    7

#ifdef __cplusplus
}
#endif
//완료된 작업:
//항목	                        결과
//app_nvram.c	                mflash → Zephyr NVS (ID 1) 포팅 완료
//matter_prefs.c	            mflash → Zephyr NVS (ID 2) 포팅 완료
//nvs_backend.c/.h	            공유 NVS 인스턴스 신규 생성
//CONFIG_BUILD_ONLY_NO_BLOBS	제거 (BLE 정상화)
//CONFIG_NVS_INIT_BAD_MEMORY_REGION=y	factory 데이터 자동 클리어
//storage_partition	            0x18620000 (4KB × 3 sectors) 정상 마운트
//matter_hub_init	            VID=65521, PID=32769, fabric=0 state=READY
//동작 순서:

//NVRAM에 유효한 credentials가 있으면 → NVRAM 값으로 WiFi 연결
//NVRAM이 비어있거나 invalid이면 → 하드코딩 값으로 WiFi 연결
//SSID: doctor supply
//PW: doctor007@@
//Security: WPA2
//Host: 192.168.0.64:3000
//콘솔에서 APP: using hardcoded wifi credentials 메시지로 어느 경로로 연결됐는지 확인할 수 있습니다.
//나중에 BLE provisioning으로 NVRAM에 실제 credentials를 저장하면 자동으로 NVRAM 값이 우선 사용됩니다. 임시 코드를 제거할 때는 /* TEMP: */ 블록을 printf("APP: no wifi creds, waiting for BLE prov\n"); 으로 교체하면 됩니다.


//# debug
//west build -b frdm_rw612 -d frdm_rw612_blank/debug frdm_rw612_blank

//# release (최적화 적용)
//west build -b frdm_rw612 -d frdm_rw612_blank/release frdm_rw612_blank -- -DCONFIG_LOG_DEFAULT_LEVEL=0 -DCONFIG_DEBUG_OPTIMIZATIONS=n

#endif /* APP_MAIN_H */
