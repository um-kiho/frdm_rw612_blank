/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared Zephyr NVS filesystem instance (storage_partition).
 * All modules that need persistent storage use app_nvs_init() +
 * app_nvs_get() so only one nvs_fs is mounted per partition.
 */

#ifndef APP_NVS_BACKEND_H
#define APP_NVS_BACKEND_H

#include <zephyr/fs/nvs.h>

/* NVS entry IDs — unique per application module. */
#define APP_NVS_ID_WIFI_CRED     1u   /* app_nvram_data_t        */
#define APP_NVS_ID_MATTER_PREFS  2u   /* matter_prefs_payload_t  */

/*
 * Mount the NVS filesystem on storage_partition if not already done.
 * Idempotent — safe to call from multiple modules.
 * Returns 0 on success, negative errno on failure.
 */
int app_nvs_init(void);

/* Return the shared nvs_fs pointer (valid after app_nvs_init() == 0). */
struct nvs_fs *app_nvs_get(void);

#endif /* APP_NVS_BACKEND_H */
