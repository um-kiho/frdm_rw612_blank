/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Matter device identity — compile-time 또는 인증 빌드에서 오버라이드.
 *
 * 테스트용 CSA Vendor ID 0xFFF1 은 테스트 제품 규격에 따라 교체해야 합니다.
 */

#ifndef APP_MATTER_DEVICE_INFO_H
#define APP_MATTER_DEVICE_INFO_H

#include <stddef.h>
#include <stdint.h>

#ifndef APP_MATTER_VENDOR_ID
#define APP_MATTER_VENDOR_ID  0xFFF1u /* Test VID — replace before production */
#endif
#ifndef APP_MATTER_PRODUCT_ID
#define APP_MATTER_PRODUCT_ID 0x8001u
#endif
#ifndef APP_MATTER_HARDWARE_VERSION
#define APP_MATTER_HARDWARE_VERSION  1u
#endif
#ifndef APP_MATTER_SOFTWARE_VERSION
#define APP_MATTER_SOFTWARE_VERSION  0x0001u
#endif

#ifndef APP_MATTER_DEVICE_NAME_MAX_LEN
#define APP_MATTER_DEVICE_NAME_MAX_LEN  31u
#endif

typedef struct matter_device_descriptor {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t hardware_version;
    uint16_t software_version;
    char     friendly_name[APP_MATTER_DEVICE_NAME_MAX_LEN + 1u];
} matter_device_descriptor_t;

#endif /* APP_MATTER_DEVICE_INFO_H */
