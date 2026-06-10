/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * HTTP pull-mode OTA with:
 *   - Resume     : "Range: bytes=N-" when an existing OTA session is mid-way.
 *   - Redirect   : Up to APP_OTA_HTTP_MAX_REDIRECTS 3xx hops with Location.
 *   - Signed     : Optional second download of a header.bin blob that is
 *                  handed to app_ota_commit(header, header_len).
 *
 * High-level flow (http_task):
 *   1. snapshot OTA status -> compute resume_from
 *   2. download image:
 *        http_get(image_url, range=resume_from,
 *                 pre_stream = open_ota_session_if_needed,
 *                 sink       = ota_chunk_sink)
 *      pre_stream gets (status, total, is_206) and either opens/restarts
 *      the OTA session, or accepts the existing one for a 206 resume.
 *   3. if header_url set: http_get(header_url, range=0, sink=buffer)
 *   4. if auto_commit:   app_ota_commit(hp, hl) + reboot
 */

#include "app_ota_http.h"
#include "app_ota.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <zephyr/kernel.h>

#include <zephyr/net/socket.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ota_http, LOG_LEVEL_INF);

/* ------------------------------------------------------------------------- *
 * URL handling
 * ------------------------------------------------------------------------- */
typedef struct url_parts {
    char     host[64];
    char     path[160];
    uint16_t port;
} url_parts_t;

static int parse_url(const char *url, url_parts_t *out)
{
    if (url == NULL || out == NULL) return -1;
    memset(out, 0, sizeof(*out));
    out->port = 80u;

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)       p += 7;
    else if (strncmp(p, "https://", 8) == 0) return -2; /* TLS: TODO */

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon != NULL && (slash == NULL || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen == 0u || hlen >= sizeof(out->host)) return -3;
        memcpy(out->host, p, hlen);
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535) return -4;
        out->port = (uint16_t)port;
        p = slash ? slash : (colon + strlen(colon));
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen == 0u || hlen >= sizeof(out->host)) return -3;
        memcpy(out->host, p, hlen);
        p = slash ? slash : (p + hlen);
    }

    if (*p == '\0') {
        out->path[0] = '/'; out->path[1] = '\0';
    } else {
        size_t plen = strlen(p);
        if (plen >= sizeof(out->path)) return -5;
        memcpy(out->path, p, plen);
    }
    return 0;
}

static int resolve_host(const char *host, struct sockaddr_in *out)
{
    out->sin_family = AF_INET;
    if (zsock_inet_pton(AF_INET, host, &out->sin_addr) == 1) {
        return 0;
    }

    LOG_ERR("host '%s' is not a numeric IPv4 address", host);
    return -1;
}

static int send_all(int s, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0u) {
        int n = zsock_send(s, p, len, 0);
        if (n <= 0) return -1;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------------- *
 * HTTP response header parsing
 * ------------------------------------------------------------------------- */
typedef struct http_response {
    int      status;
    uint32_t content_length;
    bool     has_content_length;
    uint32_t range_start;
    uint32_t range_end;
    uint32_t range_total;
    bool     has_content_range;
    char     location[APP_OTA_HTTP_URL_MAX + 1];
    bool     has_location;
    bool     is_chunked;        /* Transfer-Encoding: chunked  */
} http_response_t;

static int hdr_match(const char *line, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < n; ++i) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return line[n] == ':';
}

static const char *hdr_value(const char *line)
{
    const char *v = strchr(line, ':');
    if (v == NULL) return NULL;
    v++;
    while (*v == ' ' || *v == '\t') v++;
    return v;
}

static void hdr_copy_line(const char *line, const char *line_end,
                          char *out, size_t cap)
{
    const char *v = hdr_value(line);
    if (v == NULL || v >= line_end) return;
    const char *e = line_end;
    while (e > v && (e[-1] == '\r' || e[-1] == '\n')) e--;
    size_t n = (size_t)(e - v);
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n);
    out[n] = '\0';
}

/* Returns header byte count on success, -1 if needs more bytes,
 * <-1 if malformed. */
static int parse_response_headers(const char *buf, size_t buf_len,
                                  http_response_t *out)
{
    if (buf_len < 4u) return -1;
    const char *end = NULL;
    for (size_t i = 0; i + 3 < buf_len; ++i) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            end = buf + i + 4;
            break;
        }
    }
    if (end == NULL) return -1;

    memset(out, 0, sizeof(*out));

    if (strncmp(buf, "HTTP/1.", 7) != 0) return -2;
    const char *sp1 = strchr(buf, ' ');
    if (sp1 == NULL) return -3;
    out->status = (int)strtol(sp1 + 1, NULL, 10);

    const char *line = strstr(buf, "\r\n");
    if (line == NULL) return -4;
    line += 2;

    while (line < end - 2) {
        const char *nl = strstr(line, "\r\n");
        if (nl == NULL) break;

        if (hdr_match(line, "content-length")) {
            const char *v = hdr_value(line);
            if (v != NULL) {
                out->content_length     = (uint32_t)strtoul(v, NULL, 10);
                out->has_content_length = true;
            }
        } else if (hdr_match(line, "content-range")) {
            /* "Content-Range: bytes start-end/total" */
            const char *v = hdr_value(line);
            if (v != NULL) {
                if (strncmp(v, "bytes ", 6) == 0) v += 6;
                if (strncmp(v, "bytes=", 6) == 0) v += 6;
                char *cur = (char *)v;
                uint32_t start = (uint32_t)strtoul(cur, &cur, 10);
                if (*cur == '-') {
                    cur++;
                    uint32_t e2 = (uint32_t)strtoul(cur, &cur, 10);
                    uint32_t total = 0;
                    if (*cur == '/') {
                        cur++;
                        total = (uint32_t)strtoul(cur, NULL, 10);
                    }
                    out->range_start  = start;
                    out->range_end    = e2;
                    out->range_total  = total;
                    out->has_content_range = true;
                }
            }
        } else if (hdr_match(line, "location")) {
            hdr_copy_line(line, nl, out->location, sizeof(out->location));
            out->has_location = (out->location[0] != '\0');
        } else if (hdr_match(line, "transfer-encoding")) {
            /* Multiple values comma-separated; we only care about "chunked".
             * Per RFC 7230 §3.3.1 chunked must be the final encoding. */
            const char *v = hdr_value(line);
            if (v != NULL) {
                const char *e2 = nl;
                while (v < e2) {
                    if ((e2 - v) >= 7 &&
                        (v[0] == 'c' || v[0] == 'C') &&
                        (v[1] == 'h' || v[1] == 'H') &&
                        (v[2] == 'u' || v[2] == 'U') &&
                        (v[3] == 'n' || v[3] == 'N') &&
                        (v[4] == 'k' || v[4] == 'K') &&
                        (v[5] == 'e' || v[5] == 'E') &&
                        (v[6] == 'd' || v[6] == 'D')) {
                        out->is_chunked = true;
                        break;
                    }
                    v++;
                }
            }
        }
        line = nl + 2;
    }
    return (int)(end - buf);
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

/* ------------------------------------------------------------------------- *
 * Byte stream over (prefilled buffer + socket)
 *
 * Header parsing usually overshoots into the body by a few bytes. We hand
 * those leftover bytes plus the open socket to the body reader as a
 * unified stream so the body parser does not need to special-case them.
 * ------------------------------------------------------------------------- */
typedef struct byte_stream {
    int            sock;
    const uint8_t *prebuf;
    size_t         pre_len;
    size_t         pre_off;
} byte_stream_t;

/* Module state is referenced by the stream helpers below, so declare it here.
 * The storage definition remains later in the file with the other module state. */
static volatile bool                  s_running;
static volatile app_ota_http_state_t  s_state;
static volatile uint32_t              s_dl_seen;   /* live byte counter (diagnostics) */
static volatile uint32_t              s_dl_total;  /* live total bytes (diagnostics) */

static int bs_read_some(byte_stream_t *s, uint8_t *out, size_t want, size_t *got)
{
    if (s->pre_off < s->pre_len) {
        size_t avail = s->pre_len - s->pre_off;
        size_t take  = (avail < want) ? avail : want;
        memcpy(out, s->prebuf + s->pre_off, take);
        s->pre_off += take;
        *got = take;
        return 0;
    }
    int n = zsock_recv(s->sock, out, (int)want, 0);
    if (n <= 0) return -1;
    *got = (size_t)n;
    return 0;
}

static int bs_read_exact(byte_stream_t *s, uint8_t *out, size_t want)
{
    while (want > 0u) {
        if (!s_running) return -1;
        size_t got = 0u;
        if (bs_read_some(s, out, want, &got) != 0) return -2;
        out  += got;
        want -= got;
    }
    return 0;
}

/* Read one CRLF-terminated line (CRLF stripped). On overflow we still
 * consume bytes until CRLF but return -2 so caller can fail cleanly. */
static int bs_read_line(byte_stream_t *s, char *line, size_t cap)
{
    size_t n = 0u;
    int    prev_cr = 0;
    for (;;) {
        if (!s_running) return -1;
        uint8_t c;
        size_t  got = 0u;
        if (bs_read_some(s, &c, 1u, &got) != 0) return -3;
        if (c == '\n' && prev_cr) {
            if (n > 0u && line[n - 1] == '\r') n--;   /* strip CR */
            if (n >= cap) return -2;
            line[n] = '\0';
            return 0;
        }
        if (n < cap - 1u) {
            line[n++] = (char)c;
        } else {
            /* line too long: keep draining but mark overflow */
            n = cap;  /* sentinel */
        }
        prev_cr = (c == '\r');
        if (n == cap) {
            /* drain until CRLF, then bail */
            while (s_running) {
                uint8_t d;
                if (bs_read_some(s, &d, 1u, &got) != 0) return -3;
                if (d == '\n' && prev_cr) {
                    line[cap - 1] = '\0';
                    return -2;
                }
                prev_cr = (d == '\r');
            }
            return -1;
        }
    }
}

/* ------------------------------------------------------------------------- *
 * Body sink + pre-stream hook
 * ------------------------------------------------------------------------- */
typedef enum sink_mode {
    SINK_OTA_CHUNK = 0,    /* feeds app_ota_chunk()         */
    SINK_BUFFER    = 1,    /* copies into a RAM buffer      */
} sink_mode_t;

typedef struct sink_ctx {
    sink_mode_t mode;
    uint32_t    ota_offset;
    uint8_t    *buf;
    size_t      buf_cap;
    size_t      buf_filled;
} sink_ctx_t;

static int sink_write(sink_ctx_t *s, const uint8_t *data, size_t len)
{
    if (s->mode == SINK_OTA_CHUNK) {
        int rc = app_ota_chunk(s->ota_offset, data, len);
        if (rc != 0) return rc;
        s->ota_offset += (uint32_t)len;
        return 0;
    } else {
        if (s->buf_filled + len > s->buf_cap) return -1;
        memcpy(s->buf + s->buf_filled, data, len);
        s->buf_filled += len;
        return 0;
    }
}

/* Called once between header parsing and the first body byte. Used by the
 * image-download path to (a) decide whether resume worked, and (b) open or
 * restart the OTA session with the right total size. Return 0 to proceed. */
typedef int (*pre_stream_cb_t)(const http_response_t *rsp,
                               uint32_t requested_range,
                               sink_ctx_t *sink, void *user);

/* ------------------------------------------------------------------------- *
 * Body stream helpers
 *
 * stream_fixed_body  : Content-Length / Content-Range bounded body.
 * stream_chunked_body: Transfer-Encoding: chunked decoder
 *                      FSM = [READ_SIZE_LINE] -> [READ_DATA] -> [READ_CRLF]
 *                            -> repeat, until size==0 -> [TRAILER_LINES] -> done.
 * ------------------------------------------------------------------------- */
static int stream_fixed_body(byte_stream_t *bs, sink_ctx_t *sink,
                             uint32_t body_total)
{
    static uint8_t rxbuf[APP_OTA_HTTP_RX_CHUNK];
    uint32_t seen = 0u;
    s_dl_seen  = 0u;
    s_dl_total = body_total;
    /* 핫패스 로깅 없음: 99% hang의 원인이 막판 버스트 구간의 과다 deferred 로그였다.
     * 진행률은 ota_heartbeat_task가 5초마다 s_dl_seen으로 보고한다. */
    LOG_INF("body stream begin total=%u", (unsigned)body_total);
    while (seen < body_total) {
        if (!s_running) return -1;
        size_t want = body_total - seen;
        if (want > sizeof(rxbuf)) want = sizeof(rxbuf);
        size_t got = 0u;
        if (bs_read_some(bs, rxbuf, want, &got) != 0) {
            LOG_ERR("recv stalled %u/%u",
                   (unsigned)seen, (unsigned)body_total);
            return -2;
        }
        int wrc = sink_write(sink, rxbuf, got);
        if (wrc != 0) {
            LOG_ERR("sink_write failed at %u (+%u) rc=%d",
                   (unsigned)seen, (unsigned)got, wrc);
            return -3;
        }
        seen += (uint32_t)got;
        s_dl_seen = seen;
        /* Yield to let WiFi/TCP RX work. Without this the OTA task can starve
         * the network stack and the download stalls mid-stream. */
        k_yield();
    }
    LOG_INF("body stream complete %u B", (unsigned)seen);
    return 0;
}

static int stream_chunked_body(byte_stream_t *bs, sink_ctx_t *sink)
{
    static uint8_t rxbuf[APP_OTA_HTTP_RX_CHUNK];
    char           line[40];

    for (;;) {
        if (!s_running) return -1;

        /* --- chunk-size line --- */
        int rc = bs_read_line(bs, line, sizeof(line));
        if (rc != 0) return -2;

        /* hex size, optionally followed by ";<chunk-ext>" */
        char *endp = NULL;
        unsigned long sz = strtoul(line, &endp, 16);
        if (endp == line) return -3;             /* not even one hex digit */

        if (sz == 0u) {
            /* Trailer section: zero or more header lines, terminated by
             * an empty line. We discard them. */
            for (;;) {
                rc = bs_read_line(bs, line, sizeof(line));
                if (rc == -2) continue;          /* overlong trailer: ignore */
                if (rc != 0)  return -4;
                if (line[0] == '\0') break;      /* CRLF -> end of body */
            }
            return 0;
        }

        /* --- chunk data --- */
        while (sz > 0u) {
            if (!s_running) return -5;
            size_t want = (sz > sizeof(rxbuf)) ? sizeof(rxbuf) : (size_t)sz;
            size_t got  = 0u;
            if (bs_read_some(bs, rxbuf, want, &got) != 0) return -6;
            if (sink_write(sink, rxbuf, got) != 0)        return -7;
            sz -= (unsigned long)got;
        }

        /* --- CRLF terminator after data --- */
        uint8_t crlf[2];
        if (bs_read_exact(bs, crlf, 2u) != 0) return -8;
        if (crlf[0] != '\r' || crlf[1] != '\n') return -9;
    }
}

static char    s_image_url [APP_OTA_HTTP_URL_MAX + 1];
static char    s_header_url[APP_OTA_HTTP_URL_MAX + 1];
static bool    s_has_header_url;
static bool    s_auto_commit;
static uint8_t s_signed_hdr[APP_OTA_HTTP_SIGNED_HEADER_MAX];

/* ------------------------------------------------------------------------- *
 * Single HTTP request (no redirect handling here; see http_get).
 *
 *   range_from > 0  -> add "Range: bytes=N-"
 *   pre_cb          -> if non-NULL, invoked after the response headers are
 *                      parsed (only when status is final, i.e. not 3xx) and
 *                      before any body byte enters the sink.
 *
 * Returns:
 *    0 on full success
 *   +1 on redirect (call site should follow rsp_out->location)
 *   <0 on hard failure
 * ------------------------------------------------------------------------- */
typedef struct http_req {
    const char     *url;
    uint32_t        range_from;
    sink_ctx_t     *sink;
    pre_stream_cb_t pre_cb;
    void           *pre_arg;
    /* on return */
    http_response_t rsp;
} http_req_t;

static int http_request_once(http_req_t *r)
{
    int                sock = -1;
    int                rc   = -1;
    url_parts_t        u;
    struct sockaddr_in addr;
    static char        hdrbuf[APP_OTA_HTTP_HEADER_MAX];

    memset(&r->rsp, 0, sizeof(r->rsp));

    s_state = APP_OTA_HTTP_RESOLVING;
    if (parse_url(r->url, &u) != 0)        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_port = htons(u.port);
    if (resolve_host(u.host, &addr) != 0)  return -2;

    s_state = APP_OTA_HTTP_CONNECTING;
    LOG_INF("connect %s:%u path=%s", u.host, u.port, u.path);
    sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        LOG_ERR("socket() failed errno=%d", errno);
        return -3;
    }

    struct timeval tv;
    tv.tv_sec  = (APP_OTA_HTTP_RECV_TIMEOUT_MS / 1000);
    tv.tv_usec = (APP_OTA_HTTP_RECV_TIMEOUT_MS % 1000) * 1000;
    (void)zsock_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)zsock_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERR("connect %s:%u failed errno=%d", u.host, u.port, errno);
        rc = -4; goto out;
    }
    LOG_INF("connected");

    s_state = APP_OTA_HTTP_REQUESTING;
    int n;
    if (r->range_from > 0u) {
        n = snprintf(hdrbuf, sizeof(hdrbuf),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: zcube/1\r\n"
                     "Connection: close\r\n"
                     "Accept: */*\r\n"
                     "Range: bytes=%u-\r\n\r\n",
                     u.path, u.host, (unsigned)r->range_from);
    } else {
        n = snprintf(hdrbuf, sizeof(hdrbuf),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: zcube/1\r\n"
                     "Connection: close\r\n"
                     "Accept: */*\r\n\r\n",
                     u.path, u.host);
    }
    if (n <= 0 || (size_t)n >= sizeof(hdrbuf)) { rc = -5; goto out; }
    if (send_all(sock, hdrbuf, (size_t)n) != 0) {
        LOG_ERR("send request failed errno=%d", errno);
        rc = -6; goto out;
    }
    LOG_INF("request sent (%d B), waiting response", n);

    /* Read until \r\n\r\n. */
    size_t hdr_filled = 0;
    int    parsed     = -1;
    while (parsed == -1) {
        if (hdr_filled >= sizeof(hdrbuf)) { rc = -7; goto out; }
        int got = zsock_recv(sock, hdrbuf + hdr_filled,
                            sizeof(hdrbuf) - hdr_filled, 0);
        if (got <= 0) {
            LOG_ERR("recv response failed got=%d errno=%d filled=%u",
                    got, errno, (unsigned)hdr_filled);
            rc = -8; goto out;
        }
        hdr_filled += (size_t)got;
        parsed = parse_response_headers(hdrbuf, hdr_filled, &r->rsp);
        if (parsed < -1) { rc = -9; goto out; }
    }

    LOG_INF("%s -> status=%d cl=%u cr=%d te=%s",
           u.path, r->rsp.status,
           (unsigned)r->rsp.content_length,
           r->rsp.has_content_range ? 1 : 0,
           r->rsp.is_chunked ? "chunked" : "identity");

    /* Redirect -> caller follows Location. */
    if (is_redirect(r->rsp.status)) {
        rc = (r->rsp.has_location ? 1 : -10);
        goto out;
    }

    /* Final status: invoke pre_stream hook so the caller can open/restart
     * the OTA session before the very first body byte. */
    if (r->pre_cb != NULL) {
        int prc = r->pre_cb(&r->rsp, r->range_from, r->sink, r->pre_arg);
        if (prc != 0) { rc = -11; goto out; }
    }

    s_state = (r->sink->mode == SINK_OTA_CHUNK) ? APP_OTA_HTTP_DOWNLOADING
                                                : APP_OTA_HTTP_HEADER_DL;

    /* Build a unified byte stream over (header leftover + socket). */
    byte_stream_t bs;
    bs.sock    = sock;
    bs.prebuf  = (const uint8_t *)(hdrbuf + parsed);
    bs.pre_len = hdr_filled - (size_t)parsed;
    bs.pre_off = 0u;

    if (r->rsp.is_chunked) {
        /* Chunked: total size unknown. Decoder reads until 0-length chunk. */
        if (stream_chunked_body(&bs, r->sink) != 0) { rc = -14; goto out; }
    } else {
        /* Fixed length: derive body_total from CL / CR. */
        uint32_t body_total;
        if (r->rsp.status == 206 && r->rsp.has_content_range) {
            body_total = r->rsp.range_end + 1u - r->rsp.range_start;
        } else if ((r->rsp.status == 200 || r->rsp.status == 201) &&
                   r->rsp.has_content_length) {
            body_total = r->rsp.content_length;
        } else {
            /* No length, no chunked: cannot frame. */
            rc = -12; goto out;
        }
        if (body_total == 0u) { rc = -13; goto out; }
        if (stream_fixed_body(&bs, r->sink, body_total) != 0) {
            rc = -15; goto out;
        }
    }
    rc = 0;

out:
    if (sock >= 0) zsock_close(sock);
    return rc;
}

/* Redirect-following wrapper. Re-uses the same http_req but walks Location
 * up to APP_OTA_HTTP_MAX_REDIRECTS times. */
static int http_request(http_req_t *r)
{
    char working_url[APP_OTA_HTTP_URL_MAX + 1];
    size_t ulen = strlen(r->url);
    if (ulen > APP_OTA_HTTP_URL_MAX) return -1;
    memcpy(working_url, r->url, ulen + 1);

    for (unsigned hop = 0; hop <= APP_OTA_HTTP_MAX_REDIRECTS; ++hop) {
        http_req_t req = *r;
        req.url = working_url;

        int rc = http_request_once(&req);
        r->rsp = req.rsp;
        if (rc == 0)  return 0;
        if (rc != 1)  return rc;
        /* redirect */
        if (hop == APP_OTA_HTTP_MAX_REDIRECTS) {
            LOG_ERR("too many redirects");
            return -1;
        }
        LOG_INF("redirect -> %s", r->rsp.location);
        size_t rlen = strlen(r->rsp.location);
        if (rlen > APP_OTA_HTTP_URL_MAX) return -101;
        memcpy(working_url, r->rsp.location, rlen + 1);
        r->rsp.has_location = false;
    }
    return -102;
}

/* ------------------------------------------------------------------------- *
 * Image-download pre-stream hook
 *
 * Three cases to handle after we've seen the response headers:
 *   A. Fresh download (no resume requested): expect 200 + Content-Length.
 *      Open a brand new OTA session at offset 0.
 *   B. Resume requested and 206 received: leave the existing OTA session
 *      open and align ota_offset to range_start.
 *   C. Resume requested but 200 returned (server ignored Range): abort the
 *      existing session and start over with the new total.
 * ------------------------------------------------------------------------- */
typedef struct image_pre_arg {
    bool resume_attempted;
    bool resume_succeeded;
    bool session_restarted;
} image_pre_arg_t;

static int image_pre_stream(const http_response_t *rsp,
                            uint32_t requested_range,
                            sink_ctx_t *sink, void *user)
{
    image_pre_arg_t *ip = (image_pre_arg_t *)user;

    /* Transfer-Encoding: chunked is incompatible with OTA image streaming.
     * app_ota_begin(total) needs the total image size up front, but a
     * chunked response gives us no size header. We fail here cleanly.
     *
     * TODO: add a HEAD-request fallback that pre-fetches Content-Length
     *       from a HEAD on the same URL, then re-issues the GET. Until
     *       that is in place, callers must point image_url at a server
     *       that returns Content-Length (most static hosts do).
     */
    if (rsp->is_chunked) {
        LOG_ERR("image URL returned chunked encoding -> unsupported");
        return -10;
    }

    if (requested_range == 0u) {
        if (rsp->status != 200 || !rsp->has_content_length) return -1;
        /* Fresh: open new session. */
        if (app_ota_begin(rsp->content_length) != 0) return -2;
        sink->ota_offset = 0u;
        return 0;
    }

    /* requested_range > 0: resume attempt */
    ip->resume_attempted = true;

    if (rsp->status == 206 && rsp->has_content_range &&
        rsp->range_start == requested_range) {
        /* Resume worked: keep existing session as-is. */
        sink->ota_offset = rsp->range_start;
        ip->resume_succeeded = true;
        return 0;
    }
    if (rsp->status == 200 && rsp->has_content_length) {
        /* Server ignored Range: restart the OTA session. */
        LOG_INF("server ignored Range, restarting session");
        app_ota_abort();
        if (app_ota_begin(rsp->content_length) != 0) return -3;
        sink->ota_offset = 0u;
        ip->session_restarted = true;
        return 0;
    }
    return -4;
}

/* ------------------------------------------------------------------------- *
 * The download task
 * ------------------------------------------------------------------------- */
static void ota_heartbeat_task(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;
    uint32_t last_seen = 0u;
    uint32_t same_count = 0u;
    while (1) {
        k_sleep(K_SECONDS(5));
        if (!s_running) {
            last_seen  = 0u;
            same_count = 0u;
            continue;
        }
        uint32_t seen  = s_dl_seen;
        uint32_t total = s_dl_total;
        /* 5초마다 진행률/정지 보고 (per-chunk 로그를 없앤 대신 이게 진행률 소스). */
        if (seen == last_seen) {
            same_count++;
            LOG_WRN("HB: state=%d seen=%u/%u STALL (%u x5s)",
                    (int)s_state, (unsigned)seen, (unsigned)total,
                    (unsigned)same_count);
        } else {
            LOG_INF("HB: state=%d seen=%u/%u",
                    (int)s_state, (unsigned)seen, (unsigned)total);
            same_count = 0u;
        }
        last_seen = seen;
    }
}

static void http_task(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;

    /* Stage 1: image.bin */
    app_ota_status_t st;
    app_ota_get_status(&st);

    uint32_t resume_from = 0u;
    if (st.state == APP_OTA_STATE_WRITING && st.bytes_received > 0u) {
        resume_from = st.bytes_received;
        LOG_INF("resume from %u/%u",
               (unsigned)resume_from, (unsigned)st.total_size);
    }

    sink_ctx_t img_sink = { 0 };
    img_sink.mode       = SINK_OTA_CHUNK;
    img_sink.ota_offset = resume_from;     /* may be overwritten by pre_cb */

    image_pre_arg_t ipa = { 0 };

    http_req_t req = { 0 };
    req.url        = s_image_url;
    req.range_from = resume_from;
    req.sink       = &img_sink;
    req.pre_cb     = image_pre_stream;
    req.pre_arg    = &ipa;

    int img_rc = http_request(&req);
    if (img_rc != 0) {
        LOG_ERR("image download failed rc=%d state=%d",
                img_rc, (int)s_state);
        goto fail;
    }
    LOG_INF("image download ok, bytes=%u", (unsigned)img_sink.ota_offset);

    /* Stage 2: optional signed-image header.bin */
    size_t header_len = 0u;
    if (s_has_header_url) {
        sink_ctx_t hdr_sink = { 0 };
        hdr_sink.mode    = SINK_BUFFER;
        hdr_sink.buf     = s_signed_hdr;
        hdr_sink.buf_cap = sizeof(s_signed_hdr);

        http_req_t hr = { 0 };
        hr.url        = s_header_url;
        hr.range_from = 0u;
        hr.sink       = &hdr_sink;
        hr.pre_cb     = NULL;

        s_state = APP_OTA_HTTP_HEADER_DL;
        if (http_request(&hr) != 0) {
            LOG_ERR("header.bin download failed");
            goto fail;
        }
        header_len = hdr_sink.buf_filled;
        LOG_INF("header.bin %u B fetched", (unsigned)header_len);
    }

    /* Stage 3: commit (optional) */
    if (s_auto_commit) {
        s_state = APP_OTA_HTTP_COMMITTING;
        const uint8_t *hp = (header_len > 0u) ? s_signed_hdr : NULL;
        if (app_ota_commit(hp, header_len) != 0) {
            LOG_ERR("commit failed");
            goto fail;
        }
        s_state   = APP_OTA_HTTP_DONE;
        s_running = false;
        app_ota_reboot_after_ms(800u);
        return;
    }

    /* Manual-commit mode: client (BLE phone / TCP / serial) sends COMMIT
     * separately. The OTA session stays in WRITING; once bytes_received
     * equals total_size, app_ota_commit() will accept the request. */
    s_state   = APP_OTA_HTTP_DONE;
    s_running = false;
    return;

fail:
    app_ota_abort();
    s_state   = APP_OTA_HTTP_FAILED;
    s_running = false;
}

/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */
int app_ota_http_start(const app_ota_http_opts_t *opts)
{
    if (opts == NULL || opts->image_url == NULL) return -1;
    if (s_running)                                return -2;

    /* Ensure the OTA storage backend is initialized exactly once. The BLE
     * OTA service also calls this when it is built; app_ota_init is
     * idempotent, so calling it here covers builds that do not include
     * svc_ota (e.g. TCP-only OTA trigger). */
    static bool s_ota_core_inited = false;
    if (!s_ota_core_inited) {
        int irc = app_ota_init(NULL, NULL);
        if (irc != 0) {
            LOG_ERR("app_ota_init failed rc=%d", irc);
            return -10;
        }
        s_ota_core_inited = true;
    }

    url_parts_t tmp;
    if (parse_url(opts->image_url, &tmp) != 0) return -3;

    size_t in = strlen(opts->image_url);
    if (in > APP_OTA_HTTP_URL_MAX) return -4;
    memcpy(s_image_url, opts->image_url, in + 1);

    if (opts->header_url != NULL && opts->header_url[0] != '\0') {
        if (parse_url(opts->header_url, &tmp) != 0) return -5;
        size_t hn = strlen(opts->header_url);
        if (hn > APP_OTA_HTTP_URL_MAX) return -6;
        memcpy(s_header_url, opts->header_url, hn + 1);
        s_has_header_url = true;
    } else {
        s_has_header_url = false;
        s_header_url[0]  = '\0';
    }

    s_auto_commit = opts->auto_commit;
    s_running     = true;
    s_state       = APP_OTA_HTTP_RESOLVING;

    /* Diagnostics heartbeat: an independent low-priority thread that prints
     * the live byte counter every 5 s. If the main OTA task is blocked in
     * recv()/flash_area_write(), the byte counter freezes but this thread
     * keeps logging, so we can tell hang-in-syscall from total system hang. */
    static K_THREAD_STACK_DEFINE(ota_hb_stack, 768);
    static struct k_thread ota_hb_data;
    static bool s_hb_started = false;
    if (!s_hb_started) {
        k_thread_create(&ota_hb_data, ota_hb_stack,
                        K_THREAD_STACK_SIZEOF(ota_hb_stack),
                        ota_heartbeat_task, NULL, NULL, NULL,
                        9, 0, K_NO_WAIT);
        k_thread_name_set(&ota_hb_data, "ota_hb");
        s_hb_started = true;
    }

    static K_THREAD_STACK_DEFINE(http_task_stack, APP_OTA_HTTP_TASK_STACK);
    static struct k_thread http_task_data;
    k_tid_t tid = k_thread_create(&http_task_data, http_task_stack,
                                   K_THREAD_STACK_SIZEOF(http_task_stack),
                                   http_task, NULL, NULL, NULL,
                                   APP_OTA_HTTP_TASK_PRIO, 0, K_NO_WAIT);
    if (tid == NULL) {
        s_running = false;
        s_state   = APP_OTA_HTTP_FAILED;
        return -7;
    }
    k_thread_name_set(tid, "ota_http");
    LOG_INF("start image=%s%s%s auto_commit=%d",
           s_image_url,
           s_has_header_url ? " header=" : "",
           s_has_header_url ? s_header_url : "",
           (int)s_auto_commit);
    return 0;
}

void app_ota_http_stop(void)
{
    s_running = false;
}

bool app_ota_http_is_running(void)
{
    return s_running;
}

app_ota_http_state_t app_ota_http_get_state(void)
{
    return s_state;
}

/* ------------------------------------------------------------------------- *
 * Zephyr shell commands
 *
 *   ota start <image_url> [--commit]   - HTTP pull OTA (auto-commit if --commit)
 *   ota start <image_url> <header_url> - HTTP pull OTA with signed header.bin
 *   ota stop                           - abort running download
 *   ota status                         - print current state/progress
 *
 * Example:
 *   ota start http://172.28.176.1:8080/firmware/zephyr.bin --commit
 * ------------------------------------------------------------------------- */
#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static const char * const s_state_name[] = {
    "IDLE", "RESOLVING", "CONNECTING", "REQUESTING",
    "DOWNLOADING", "HEADER_DL", "COMMITTING", "DONE", "FAILED",
};

static int cmd_ota_start(const struct shell *sh, size_t argc, char **argv)
{
    /* argv[1] = image_url
     * argv[2] = header_url  OR  "--commit"  (optional)
     * argv[3] = "--commit"                  (optional, when argv[2] is header) */
    if (argc < 2) {
        shell_error(sh, "usage: ota start <image_url> [<header_url>] [--commit]");
        shell_error(sh, "  e.g. ota start http://172.28.176.1:8080/firmware/zephyr.bin --commit");
        return -EINVAL;
    }

    if (app_ota_http_is_running()) {
        shell_warn(sh, "OTA already running (state=%s). Use 'ota stop' first.",
                   s_state_name[app_ota_http_get_state()]);
        return -EBUSY;
    }

    app_ota_http_opts_t opts;
    opts.image_url   = argv[1];
    opts.header_url  = NULL;
    opts.auto_commit = false;

    for (size_t i = 2; i < (size_t)argc; ++i) {
        if (strcmp(argv[i], "--commit") == 0) {
            opts.auto_commit = true;
        } else {
            opts.header_url = argv[i];
        }
    }

    shell_print(sh, "OTA start: %s%s%s auto_commit=%s",
                opts.image_url,
                opts.header_url ? "  header=" : "",
                opts.header_url ? opts.header_url : "",
                opts.auto_commit ? "yes" : "no");

    int rc = app_ota_http_start(&opts);
    if (rc != 0) {
        shell_error(sh, "app_ota_http_start failed (%d)", rc);
        return rc;
    }
    shell_print(sh, "Download started.");
    return 0;
}

static int cmd_ota_stop(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    app_ota_http_stop();
    shell_print(sh, "OTA stop requested.");
    return 0;
}

static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
    (void)argc; (void)argv;
    app_ota_status_t st;
    app_ota_get_status(&st);

    app_ota_http_state_t hs = app_ota_http_get_state();
    const char *hs_name = (hs < (app_ota_http_state_t)ARRAY_SIZE(s_state_name))
                          ? s_state_name[hs] : "?";

    static const char * const s_ota_state_name[] = {
        "IDLE", "READY", "WRITING", "COMMITTED", "FAILED",
    };
    const char *os_name = (st.state < ARRAY_SIZE(s_ota_state_name))
                          ? s_ota_state_name[st.state] : "?";

    shell_print(sh, "HTTP state : %s (%s)",
                hs_name, app_ota_http_is_running() ? "running" : "idle");
    shell_print(sh, "OTA  state : %s  err=%u", os_name, (unsigned)st.last_err);
    shell_print(sh, "Progress   : %u / %u bytes",
                (unsigned)st.bytes_received, (unsigned)st.total_size);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ota,
    SHELL_CMD_ARG(start, NULL,
        "Start HTTP OTA\n"
        "  ota start <image_url> [<header_url>] [--commit]\n"
        "  e.g. ota start http://172.28.176.1:8080/firmware/zephyr.bin --commit",
        cmd_ota_start, 2, 3),
    SHELL_CMD(stop,   NULL, "Abort running OTA download", cmd_ota_stop),
    SHELL_CMD(status, NULL, "Print OTA state and progress", cmd_ota_status),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ota, &sub_ota, "OTA firmware update commands", NULL);
#endif /* CONFIG_SHELL */
