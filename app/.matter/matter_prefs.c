/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Matter preferences — Zephyr NVS backend (replaces mflash_file API).
 *
 * Payload stored as a single NVS entry (ID 2). NVS handles
 * wear-levelling; no custom header or CRC needed here.
 */

#include "matter_prefs.h"
#include "nvs_backend.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_matter_prefs, LOG_LEVEL_INF);

static void payload_default(matter_prefs_payload_t *out)
{
	memset(out, 0, sizeof(*out));
	out->commissioning_state = (uint32_t)MATTER_CMP_READY;
}

int matter_prefs_factory_default(void)
{
	matter_prefs_payload_t p;
	payload_default(&p);
	return matter_prefs_save(&p);
}

int matter_prefs_save(const matter_prefs_payload_t *payload)
{
	if (payload == NULL) {
		return -1;
	}
	if (app_nvs_init() != 0) {
		return -2;
	}

	ssize_t rc = nvs_write(app_nvs_get(), APP_NVS_ID_MATTER_PREFS,
			       payload, sizeof(*payload));
	if (rc < 0) {
		LOG_ERR("nvs_write err=%d", (int)rc);
		return -3;
	}
	return 0;
}

int matter_prefs_load(matter_prefs_payload_t *out)
{
	if (out == NULL) {
		return -1;
	}
	if (app_nvs_init() != 0) {
		payload_default(out);
		return -2;
	}

	ssize_t rc = nvs_read(app_nvs_get(), APP_NVS_ID_MATTER_PREFS,
			      out, sizeof(*out));
	if (rc == -ENOENT) {
		payload_default(out);
		return -3; /* no record — caller writes defaults */
	}
	if (rc < 0) {
		LOG_ERR("nvs_read err=%d", (int)rc);
		payload_default(out);
		return -4;
	}
	if ((size_t)rc != sizeof(*out)) {
		payload_default(out);
		return -5;
	}
	return 0;
}

bool matter_prefs_is_commissioned(void)
{
	matter_prefs_payload_t p;
	if (matter_prefs_load(&p) != 0) {
		return false;
	}
	return (p.fabric_count != 0u) &&
	       (p.commissioning_state == (uint32_t)MATTER_CMP_COMMISSIONED);
}
