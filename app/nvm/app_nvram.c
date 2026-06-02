/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * NVRAM — Zephyr NVS backend (replaces mflash_file API).
 *
 * Wi-Fi provisioning data is stored as a single NVS entry (ID 1).
 * NVS handles wear-levelling and internal consistency; no custom
 * header or CRC is needed at the application level.
 */

#include "app_nvram.h"
#include "nvs_backend.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_nvram, LOG_LEVEL_INF);

static const char * const k_allowed_security[] = {
	"OPEN",
	"WPA2",
	"WPA2_WPA3",
	"WPA3_SAE",
};

int app_nvram_init(void)
{
	return (app_nvs_init() == 0) ? APP_NVRAM_OK : APP_NVRAM_ERR_IO;
}

int app_nvram_load(app_nvram_data_t *out)
{
	if (out == NULL) {
		return APP_NVRAM_ERR_PARAM;
	}
	if (app_nvs_init() != 0) {
		return APP_NVRAM_ERR_IO;
	}

	ssize_t rc = nvs_read(app_nvs_get(), APP_NVS_ID_WIFI_CRED, out, sizeof(*out));
	if (rc == -ENOENT) {
		return APP_NVRAM_ERR_NO_RECORD;
	}
	if (rc < 0) {
		LOG_ERR("nvs_read err=%d", (int)rc);
		return APP_NVRAM_ERR_IO;
	}
	if ((size_t)rc != sizeof(*out)) {
		return APP_NVRAM_ERR_LENGTH;
	}

	/* Defensive NUL termination. */
	out->ssid    [APP_NVRAM_SSID_MAXLEN] = '\0';
	out->password[APP_NVRAM_PASS_MAXLEN] = '\0';
	out->security[APP_NVRAM_SEC_MAXLEN ] = '\0';
	out->host_ip [APP_NVRAM_HOST_MAXLEN] = '\0';
	return APP_NVRAM_OK;
}

int app_nvram_save(const app_nvram_data_t *in)
{
	if (in == NULL) {
		return APP_NVRAM_ERR_PARAM;
	}
	if (app_nvs_init() != 0) {
		return APP_NVRAM_ERR_IO;
	}

	app_nvram_data_t payload = *in;
	payload.ssid    [APP_NVRAM_SSID_MAXLEN] = '\0';
	payload.password[APP_NVRAM_PASS_MAXLEN] = '\0';
	payload.security[APP_NVRAM_SEC_MAXLEN ] = '\0';
	payload.host_ip [APP_NVRAM_HOST_MAXLEN] = '\0';
	payload._pad = 0;

	ssize_t rc = nvs_write(app_nvs_get(), APP_NVS_ID_WIFI_CRED,
			       &payload, sizeof(payload));
	if (rc < 0) {
		LOG_ERR("nvs_write err=%d", (int)rc);
		return APP_NVRAM_ERR_IO;
	}
	return APP_NVRAM_OK;
}

int app_nvram_reset(void)
{
	if (app_nvs_init() != 0) {
		return APP_NVRAM_ERR_IO;
	}
	int rc = nvs_delete(app_nvs_get(), APP_NVS_ID_WIFI_CRED);
	return (rc == 0 || rc == -ENOENT) ? APP_NVRAM_OK : APP_NVRAM_ERR_IO;
}

static bool str_nonempty_within(const char *s, uint32_t maxlen)
{
	if (s == NULL) {
		return false;
	}
	size_t n = strnlen(s, maxlen + 1u);
	return (n > 0u) && (n <= maxlen);
}

static bool security_allowed(const char *s)
{
	if (s == NULL) {
		return false;
	}
	for (size_t i = 0; i < sizeof(k_allowed_security) / sizeof(k_allowed_security[0]); ++i) {
		if (strcmp(s, k_allowed_security[i]) == 0) {
			return true;
		}
	}
	return false;
}

bool app_nvram_is_valid(const app_nvram_data_t *in)
{
	if (in == NULL) {
		return false;
	}
	if (!str_nonempty_within(in->ssid,    APP_NVRAM_SSID_MAXLEN)) {
		return false;
	}
	if (!str_nonempty_within(in->host_ip, APP_NVRAM_HOST_MAXLEN)) {
		return false;
	}
	if (!security_allowed(in->security)) {
		return false;
	}
	if (strcmp(in->security, "OPEN") != 0) {
		if (!str_nonempty_within(in->password, APP_NVRAM_PASS_MAXLEN)) {
			return false;
		}
	}
	if (in->port == 0u) {
		return false;
	}
	return true;
}
