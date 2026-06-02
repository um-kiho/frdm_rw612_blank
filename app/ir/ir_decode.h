/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * IR protocol decoder — 가전제품(에어컨·선풍기·가습기) 전용.
 *
 * TV / AV 기기 프로토콜(Samsung TV, Sony SIRC 등)은 제외.
 *
 * 지원 프로토콜
 * ─────────────
 *  IR_PROTO_NEC          38 kHz  9 ms + 4.5 ms  32-bit LSB
 *                                addr(8) ~addr(8) cmd(8) ~cmd(8)
 *                                → 선풍기·가습기·국내 가전 대부분
 *
 *  IR_PROTO_NEC_EXT      38 kHz  같은 헤더, 16-bit 어드레스 variant
 *
 *  IR_PROTO_NEC_REPEAT   38 kHz  9 ms + 2.25 ms  홀드키 반복 버스트
 *
 *  IR_PROTO_LG_AC        38 kHz  8.5 ms + 4.25 ms  28-bit LSB
 *                                addr(8) cmd(8) ~cmd(8) chk(4)
 *                                → LG 에어컨 리모컨
 *
 *  IR_PROTO_SAMSUNG_AC   38 kHz  690 µs + 17 844 µs  14/21-byte
 *                                다중 섹션 프레임 → Samsung 에어컨
 *
 * 타이밍 허용오차: 모든 마크/스페이스 ±25 %
 *
 * 사용 예:
 *   ir_decoded_t dec;
 *   if (ir_decode(syms, count, &dec) == 0) {
 *       switch (dec.proto) {
 *       case IR_PROTO_NEC:
 *           // 선풍기/가습기: addr=dec.address, cmd=dec.command
 *           break;
 *       case IR_PROTO_LG_AC:
 *           // LG 에어컨: dec.raw_bits (28-bit)
 *           break;
 *       case IR_PROTO_SAMSUNG_AC:
 *           // Samsung 에어컨: dec.raw_bytes[0..dec.byte_count-1]
 *           break;
 *       }
 *   }
 */

#ifndef APP_IR_DECODE_H
#define APP_IR_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir_tx.h"  /* for ir_symbol_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 프로토콜 식별자 ──────────────────────────────────────────────────── */
typedef enum {
    IR_PROTO_UNKNOWN      = 0,
    IR_PROTO_NEC,              /* 32-bit NEC  (선풍기·가습기·국내 가전) */
    IR_PROTO_NEC_EXT,          /* 32-bit NEC  extended (16-bit 어드레스) */
    IR_PROTO_NEC_REPEAT,       /* NEC 홀드키 반복 버스트                 */
    IR_PROTO_LG_AC,            /* 28-bit LG 에어컨                       */
    IR_PROTO_SAMSUNG_AC,       /* Samsung 에어컨 (14 or 21 bytes)        */
} ir_proto_t;

/* Samsung AC 최대 페이로드 (21 bytes). */
#define IR_DECODE_MAX_RAW_BYTES   24u

/* ── 디코딩 결과 ──────────────────────────────────────────────────────── */
typedef struct {
    ir_proto_t proto;        /* 식별된 프로토콜                            */

    /* NEC / NEC_EXT / LG_AC 공통: */
    uint16_t   address;      /* 디바이스 어드레스 (8 or 16 bit)            */
    uint8_t    command;      /* 커맨드 코드                                */

    /* SAMSUNG_AC: */
    uint8_t    raw_bytes[IR_DECODE_MAX_RAW_BYTES];
    uint8_t    byte_count;   /* raw_bytes 유효 바이트 수                   */

    /* 진단용 (프로토콜에 무관하게 항상 채워짐): */
    uint32_t   raw_bits;     /* 32/28-bit 시프트 레지스터 원시값           */
    uint8_t    bit_count;    /* 디코딩된 비트 수                           */
    bool       repeat;       /* 홀드키 반복 프레임이면 true                */
} ir_decoded_t;

/* ── 디코드 함수 ──────────────────────────────────────────────────────── */

/*
 * IR 프로토콜 식별 및 어드레스/커맨드 추출.
 *
 * 반환값:
 *   0   성공 — out->proto 에 식별 결과 설정.
 *  -1   잘못된 인자 (NULL 포인터, count==0).
 *  -2   알 수 없는 프레임 (out->proto == IR_PROTO_UNKNOWN).
 */
int ir_decode(const ir_symbol_t *syms, size_t count, ir_decoded_t *out);

/* 프로토콜 이름 문자열 (로그/디버그용). */
const char *ir_proto_name(ir_proto_t proto);

#ifdef __cplusplus
}
#endif

#endif /* APP_IR_DECODE_H */
