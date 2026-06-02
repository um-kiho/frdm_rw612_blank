/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * BLE GATT Service #5 - OTA firmware update.
 *
 * UUID base: 5a637562-eXXX-4000-8000-000000000001
 *   - Service : e600
 *   - CTRL    : e601 (write)       phone -> device, opcode + payload
 *   - STATUS  : e602 (read+notify) current state / progress / last error
 *
 * Wire format (little-endian) on the CTRL characteristic:
 *
 *   BEGIN  : 0x01  total_size:u32                         (5 B)
 *   CHUNK  : 0x02  offset:u32  data_len:u16  data[data_len] (7 + N B)
 *   COMMIT : 0x03  header[0..M]                            (1 + M B, M may be 0)
 *   ABORT  : 0x04                                          (1 B)
 *   REBOOT : 0x05                                          (1 B)  - manual,
 *                                                                   commit normally schedules its own.
 *
 * Wire format (little-endian) of STATUS notifications (10 B):
 *
 *   state:u8  last_err:u8  bytes_received:u32  total_size:u32
 */

#ifndef SVC_OTA_H
#define SVC_OTA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_OTA_OP_BEGIN      0x01u
#define APP_OTA_OP_CHUNK      0x02u
#define APP_OTA_OP_COMMIT     0x03u
#define APP_OTA_OP_ABORT      0x04u
#define APP_OTA_OP_REBOOT     0x05u
/* Pull image (and optional secure-boot header.bin) from HTTP server(s).
 *
 * Payload layout (little-endian):
 *   [op=0x06] [flags:u8] [iurl_len:u16] [image_url[iurl_len]]
 *                        [hurl_len:u16] [header_url[hurl_len]]
 *
 * flags:
 *   bit0 AUTO_COMMIT (1=commit + reboot after transfer, 0=stay in WRITING
 *                     so the client can send op 0x03 COMMIT manually)
 *   bit1..7 reserved (must be 0)
 *
 * hurl_len may be 0 (unsigned image). header_url field is then absent.
 *
 * The device handles redirects (up to APP_OTA_HTTP_MAX_REDIRECTS), and
 * resumes from app_ota's current byte offset using "Range: bytes=N-" so
 * that interrupted downloads don't have to restart. */
#define APP_OTA_OP_HTTP_PULL          0x06u
#define APP_OTA_HTTP_FLAG_AUTO_COMMIT 0x01u

/* Initialize the BLE OTA glue. Internally calls app_ota_init() and hooks the
 * status callback to publish notifications on the STATUS characteristic. */
int app_ota_svc_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_OTA_H */
