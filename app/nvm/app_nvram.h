/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * NVRAM (mflash-backed) for provisioning data:
 *   - Wi-Fi SSID / Password / Security
 *   - TCP host IP / port
 *
 * On-flash layout (single mflash file, fixed size):
 *   [ app_nvram_hdr_t { magic, version, length, crc32 } ]
 *   [ app_nvram_data_t (payload)                       ]
 *
 * Drop-in replacement (with extra host_ip/port fields) for
 *   mcuxsdk-examples-main/wifi_examples/wifi_webconfig/cred_flash_storage.{c,h}
 */

#ifndef APP_NVRAM_H
#define APP_NVRAM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_NVRAM_SSID_MAXLEN
#define APP_NVRAM_SSID_MAXLEN      32      /* IEEE 802.11 SSID max octets        */
#endif
#ifndef APP_NVRAM_PASS_MAXLEN
#define APP_NVRAM_PASS_MAXLEN      64      /* WPA2/WPA3 PSK max                  */
#endif
#ifndef APP_NVRAM_SEC_MAXLEN
#define APP_NVRAM_SEC_MAXLEN       16      /* "WPA2", "WPA3_SAE", "OPEN", ...    */
#endif
#ifndef APP_NVRAM_HOST_MAXLEN
#define APP_NVRAM_HOST_MAXLEN      64      /* IPv4 dotted string or short FQDN   */
#endif

#ifndef APP_NVRAM_FILENAME
#define APP_NVRAM_FILENAME         "appcfg.bin"
#endif

#define APP_NVRAM_MAGIC            0x5A4E5630u   /* 'ZNV0' */
#define APP_NVRAM_VERSION          1u

typedef struct app_nvram_data {
    char     ssid    [APP_NVRAM_SSID_MAXLEN + 1];
    char     password[APP_NVRAM_PASS_MAXLEN + 1];
    char     security[APP_NVRAM_SEC_MAXLEN  + 1];
    char     host_ip [APP_NVRAM_HOST_MAXLEN + 1];
    uint16_t port;
    uint16_t _pad;
} app_nvram_data_t;

typedef struct app_nvram_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
    uint32_t crc32;
} app_nvram_hdr_t;

/* Return codes (negative = error). */
#define APP_NVRAM_OK                ( 0)
#define APP_NVRAM_ERR_PARAM         (-1)
#define APP_NVRAM_ERR_IO            (-2)
#define APP_NVRAM_ERR_NO_RECORD     (-3)
#define APP_NVRAM_ERR_MAGIC         (-4)
#define APP_NVRAM_ERR_VERSION       (-5)
#define APP_NVRAM_ERR_LENGTH        (-6)
#define APP_NVRAM_ERR_CRC           (-7)

/*
 * Initialise the underlying mflash file table.
 * Safe to call more than once; subsequent calls are no-ops.
 */
int  app_nvram_init(void);

/*
 * Load NVRAM payload into *out.
 * Returns APP_NVRAM_OK on success, or one of APP_NVRAM_ERR_* values.
 */
int  app_nvram_load (app_nvram_data_t       *out);

/*
 * Save *in as a complete record (header + payload) in a single mflash write.
 */
int  app_nvram_save (const app_nvram_data_t *in);

/*
 * Invalidate the stored record so the next load() returns APP_NVRAM_ERR_MAGIC.
 */
int  app_nvram_reset(void);

/*
 * Field-level sanity checks. Use this BEFORE app_nvram_save().
 *   - ssid:    1..APP_NVRAM_SSID_MAXLEN
 *   - host_ip: 1..APP_NVRAM_HOST_MAXLEN
 *   - port:    1..65535
 *   - security: must be one of {"OPEN","WPA2","WPA2_WPA3","WPA3_SAE"} (case-sensitive)
 */
bool app_nvram_is_valid(const app_nvram_data_t *in);

#ifdef __cplusplus
}
#endif

#endif /* APP_NVRAM_H */
