/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Matter hub stub — non-volatile preferences (mflash file matter_prf.bin).
 *
 * 실제 CSA Matter/CHIP 패브릭 키는 추후 같은 레이아웃의 상위 버전으로 확장합니다.
 */

#ifndef APP_MATTER_PREFS_H
#define APP_MATTER_PREFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MATTER_PREFS_MAGIC   0x4D5A5052u /* 'MZPR' */
#define APP_MATTER_PREFS_VERSION 1u

typedef enum matter_commissioning_state {
    MATTER_CMP_IDLE = 0,
    MATTER_CMP_READY,
    MATTER_CMP_WINDOW_OPEN,
    MATTER_CMP_COMMISSIONED,
    MATTER_CMP_ERROR,
} matter_commissioning_state_t;

typedef struct matter_prefs_payload {
    /* Persisted commissioning epoch. Do **not** store MATTER_CMP_WINDOW_OPEN
     * in flash — commissioning window timing is RAM-only in this stub. */
    uint32_t commissioning_state; /* MATTER_CMP_{IDLE,READY,COMMISSIONED,ERROR} */
    uint8_t  fabric_count;      /* 디버그 스텁: 패브릭 수 의미 유사 */
    uint8_t  feature_flags;
    uint8_t  _pad[2];
    uint8_t  opaque_blob[152];  /* 향후 NOC/ICAC 등 전개용 패딩 영역 */
} matter_prefs_payload_t;

#define MATTER_PREF_FLAG_MATTER_RUNTIME_ENABLED   (1u << 0)

int  matter_prefs_save(const matter_prefs_payload_t *payload);
int  matter_prefs_load(matter_prefs_payload_t *out);
int  matter_prefs_factory_default(void);
bool matter_prefs_is_commissioned(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MATTER_PREFS_H */
