/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "nvs_backend.h"

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_nvs, LOG_LEVEL_INF);

static struct nvs_fs s_nvs;
static bool          s_mounted;

struct nvs_fs *app_nvs_get(void)
{
	return &s_nvs;
}

int app_nvs_init(void)
{
	if (s_mounted) {
		return 0;
	}

	s_nvs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(s_nvs.flash_device)) {
		LOG_ERR("NVS flash device not ready");
		return -ENODEV;
	}
	s_nvs.offset = FIXED_PARTITION_OFFSET(storage_partition);

	struct flash_pages_info info;
	int rc = flash_get_page_info_by_offs(s_nvs.flash_device, s_nvs.offset, &info);
	if (rc != 0) {
		LOG_ERR("flash_get_page_info failed: %d", rc);
		return -EIO;
	}
	s_nvs.sector_size  = info.size;
	s_nvs.sector_count = 3u;

	rc = nvs_mount(&s_nvs);
	if (rc != 0) {
		LOG_ERR("nvs_mount failed: %d", rc);
		return rc;
	}

	s_mounted = true;
	LOG_INF("NVS mounted: offset=0x%08x sector_size=%u count=%u",
		(uint32_t)s_nvs.offset, s_nvs.sector_size, s_nvs.sector_count);
	return 0;
}
