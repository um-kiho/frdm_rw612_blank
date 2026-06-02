/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * HTTP pull-mode OTA with resume, redirect-following, and signed-image
 * header download.
 *
 * Driven by either BLE (svc_ota CTRL op 0x06 HTTP_PULL) or local code:
 *   1. Caller passes image_url ("http://host[:port]/path"), optionally a
 *      header_url for a NXP secure-image commit blob, and an auto_commit
 *      flag.
 *   2. A FreeRTOS task:
 *       - Optionally issues GET with "Range: bytes=N-" when an existing
 *         OTA session is mid-flight (state==WRITING and bytes_received>0).
 *       - Follows up to APP_OTA_HTTP_MAX_REDIRECTS 3xx redirects.
 *       - Parses Content-Length / Content-Range and pumps the body into
 *         app_ota_chunk() in APP_OTA_HTTP_RX_CHUNK slices.
 *       - If header_url is set, downloads that to a small RAM buffer.
 *       - If auto_commit, calls app_ota_commit(header_bytes, header_len)
 *         and app_ota_reboot_after_ms() so the device boots the new image.
 *   3. Progress notifications come from the standard app_ota status callback
 *      (svc_ota's e602 NOTIFY).
 *
 * Limitations (1st pass):
 *   - HTTP only (HTTPS / TLS hookup is a follow-up).
 *   - Content-Length header required.
 *   - chunked transfer-encoding not implemented.
 *   - Range: only "bytes=N-" suffix-open; multi-range / If-Match ignored.
 */

#ifndef APP_OTA_HTTP_H
#define APP_OTA_HTTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_OTA_HTTP_TASK_STACK
#define APP_OTA_HTTP_TASK_STACK            4096
#endif
#ifndef APP_OTA_HTTP_TASK_PRIO
#define APP_OTA_HTTP_TASK_PRIO             3
#endif
#ifndef APP_OTA_HTTP_RX_CHUNK
#define APP_OTA_HTTP_RX_CHUNK              1024u
#endif
#ifndef APP_OTA_HTTP_HEADER_MAX
#define APP_OTA_HTTP_HEADER_MAX            1024u
#endif
#ifndef APP_OTA_HTTP_CONNECT_TIMEOUT_MS
#define APP_OTA_HTTP_CONNECT_TIMEOUT_MS    10000u
#endif
#ifndef APP_OTA_HTTP_RECV_TIMEOUT_MS
#define APP_OTA_HTTP_RECV_TIMEOUT_MS       15000u
#endif
#ifndef APP_OTA_HTTP_URL_MAX
#define APP_OTA_HTTP_URL_MAX               240u
#endif
#ifndef APP_OTA_HTTP_MAX_REDIRECTS
#define APP_OTA_HTTP_MAX_REDIRECTS         2u
#endif
#ifndef APP_OTA_HTTP_SIGNED_HEADER_MAX
#define APP_OTA_HTTP_SIGNED_HEADER_MAX     512u
#endif

typedef enum app_ota_http_state {
    APP_OTA_HTTP_IDLE         = 0,
    APP_OTA_HTTP_RESOLVING    = 1,
    APP_OTA_HTTP_CONNECTING   = 2,
    APP_OTA_HTTP_REQUESTING   = 3,
    APP_OTA_HTTP_DOWNLOADING  = 4,
    APP_OTA_HTTP_HEADER_DL    = 5,   /* downloading signed-image header.bin */
    APP_OTA_HTTP_COMMITTING   = 6,
    APP_OTA_HTTP_DONE         = 7,
    APP_OTA_HTTP_FAILED       = 8,
} app_ota_http_state_t;

typedef struct app_ota_http_opts {
    const char *image_url;       /* required */
    const char *header_url;      /* NULL = unsigned image                   */
    bool        auto_commit;     /* true: commit + reboot when transfer ok  */
} app_ota_http_opts_t;

/* Returns 0 on successful task spawn; non-zero if a download is already
 * running, an URL is malformed, or no opts/image_url was provided.
 *
 * If app_ota's current state is WRITING with bytes_received>0, the task
 * issues "Range: bytes=N-" so the existing slot1 image keeps growing. If
 * the server answers 200 (full body), the task transparently restarts the
 * OTA session with the new total. This is how the strict in-order policy
 * of app_ota_chunk() composes with HTTP byte ranges. */
int  app_ota_http_start  (const app_ota_http_opts_t *opts);
void app_ota_http_stop   (void);
bool app_ota_http_is_running(void);
app_ota_http_state_t app_ota_http_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_OTA_HTTP_H */
