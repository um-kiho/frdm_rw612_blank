/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * IR 프로토콜 디코더 — 에어컨·선풍기·가습기 전용.
 *
 * 타이밍 허용오차: ±25 % (데이터시트 ±20 % 보다 넉넉히 설정하여
 * 저가 리모컨·수신 지연 대응).
 *
 * 프로토콜별 타이밍 기준 (µs):
 *
 *  NEC:
 *    헤더 mark 9000  space 4500
 *    비트  mark  562  0-space  562  1-space 1687
 *    반복  mark 9000  space   2250  end-mark 562
 *
 *  LG 에어컨 (28-bit):
 *    헤더 mark 8500  space 4250
 *    비트  mark  560  0-space  560  1-space 1600
 *    28 비트 LSB-first: addr(8) | cmd(8) | ~cmd(8) | chk(4)
 *    체크섬 = (addr + cmd) 의 하위 4비트 합산 (브랜드마다 다를 수 있음)
 *
 *  Samsung 에어컨 (가변 길이 다중 섹션):
 *    전역 헤더 mark 690  space 17844  (매우 특징적)
 *    섹션 헤더 mark 3086  space 8864
 *    비트   mark  586  0-space  436  1-space 1432  (LSB-first)
 */

#include "ir_decode.h"

#include <string.h>

/* ---------------------------------------------------------------------- *
 * 타이밍 허용오차 ±25 %
 * ---------------------------------------------------------------------- */
static inline bool in_range(uint32_t v, uint32_t ref)
{
    return (v >= (ref * 3u / 4u)) && (v <= (ref * 5u / 4u));
}

/* ---------------------------------------------------------------------- *
 * NEC / LG 공통 비트 디코더 (LSB-first, space-encoded)
 *
 * syms[1] 부터 최대 32비트 읽음.
 * bit_mark, zero_space, one_space 는 프로토콜별로 전달.
 * 반환값: 디코딩된 비트 수.
 * ---------------------------------------------------------------------- */
static uint8_t decode_space_bits(const ir_symbol_t *syms, size_t count,
                                  uint32_t bit_mark,
                                  uint32_t zero_space, uint32_t one_space,
                                  uint8_t  max_bits,
                                  uint32_t *bits_out)
{
    *bits_out = 0u;
    uint8_t n = 0u;

    for (size_t i = 1u; i < count && n < max_bits; i++) {
        if (!in_range(syms[i].mark_us, bit_mark)) {
            break;
        }
        if (syms[i].space_us == 0u) {
            /* 마지막 스톱비트(space 미측정) → 비트값은 0, 카운트만 증가 */
            n++;
            break;
        }
        if (in_range(syms[i].space_us, zero_space)) {
            /* bit = 0 */
        } else if (in_range(syms[i].space_us, one_space)) {
            *bits_out |= (1u << n);
        } else {
            break;   /* 예상 밖의 space 폭 → 중단 */
        }
        n++;
    }
    return n;
}

/* ---------------------------------------------------------------------- *
 * Samsung AC raw 바이트 추출
 *
 * Samsung AC 비트 타이밍:
 *   bit mark 586 µs, 0-space 436 µs, 1-space 1432 µs, LSB-first
 *
 * 구조: [전역헤더] [섹션헤더 + 56비트(7byte) + 섹션푸터] × N섹션
 * 반환값: 추출된 바이트 수 (0, 7, 14, 21).
 * ---------------------------------------------------------------------- */
#define SAC_BIT_MARK     586u
#define SAC_ZERO_SPACE   436u
#define SAC_ONE_SPACE   1432u
#define SAC_SEC_MARK    3086u
#define SAC_SEC_SPACE   8864u

/* 호출 시 syms[0] = 첫 섹션 헤더(3086/8864). (전역 헤더가 있으면 호출 측에서
 * 건너뛰고 넘겨준다.) */
static uint8_t decode_samsung_ac_bytes(const ir_symbol_t *syms, size_t count,
                                        uint8_t *out, uint8_t cap)
{
    uint8_t byte_count = 0u;
    size_t  i          = 0u;   /* syms[0] = 첫 섹션 헤더 */

    while (i < count && byte_count < cap) {
        /* 섹션 헤더 확인 */
        if (!in_range(syms[i].mark_us, SAC_SEC_MARK)) break;
        if (!in_range(syms[i].space_us, SAC_SEC_SPACE)) break;
        i++;

        /* 56비트(7바이트) 수집, LSB-first */
        uint8_t bits_left = 56u;
        uint8_t cur_byte  = 0u;
        uint8_t bit_pos   = 0u;

        while (i < count && bits_left > 0u) {
            if (!in_range(syms[i].mark_us, SAC_BIT_MARK)) break;

            uint8_t bit = 0u;
            if (in_range(syms[i].space_us, SAC_ONE_SPACE)) {
                bit = 1u;
            } else if (in_range(syms[i].space_us, SAC_ZERO_SPACE) ||
                       syms[i].space_us == 0u) {
                bit = 0u;
            } else {
                break;
            }

            cur_byte = (uint8_t)(cur_byte | (uint8_t)(bit << bit_pos));
            bit_pos++;
            if (bit_pos == 8u) {
                if (byte_count < cap) out[byte_count++] = cur_byte;
                cur_byte = 0u;
                bit_pos  = 0u;
            }
            i++;
            bits_left--;
        }

        /* 섹션 푸터(bit mark) 건너뜀 */
        if (i < count && in_range(syms[i].mark_us, SAC_BIT_MARK)) i++;
    }

    return byte_count;
}

/* ---------------------------------------------------------------------- *
 * Samsung AC 섹션 독립 디코드
 *
 * 프레임 구조(섹션헤더로 시작): [헤더1 + 56비트 + 푸터1] = 58 심볼/섹션.
 * 섹션 sec_index 를 심볼 오프셋 sec_index*58 에서 따로 디코드한다. 앞 섹션이
 * 깨져도(타이밍 오류) 뒤 섹션을 독립적으로 복구할 수 있어, 리모컨 반복분에
 * 걸쳐 섹션별로 모으면 깨진 캡처에서도 완전한 프레임을 조립할 수 있다.
 *
 * 반환:  1 = 성공(out[0..6] 채움)
 *        0 = 섹션 헤더는 맞으나 비트가 깨짐(Samsung AC 이지만 이 섹션은 손상)
 *       -1 = 그 위치에 섹션 헤더 없음(프레임이 짧거나 Samsung AC 아님)
 * ---------------------------------------------------------------------- */
int ir_decode_samsung_ac_section(const ir_symbol_t *syms, size_t count,
                                 uint8_t sec_index, uint8_t out[7])
{
    size_t hdr = (size_t)sec_index * 58u;
    if (syms == NULL || out == NULL || (hdr + 57u) >= count) return -1;
    if (!in_range(syms[hdr].mark_us, SAC_SEC_MARK) ||
        !in_range(syms[hdr].space_us, SAC_SEC_SPACE)) return -1;

    uint8_t cur = 0u, bp = 0u, bc = 0u;
    for (uint8_t k = 0u; k < 56u; ++k) {
        const ir_symbol_t *s = &syms[hdr + 1u + k];
        if (!in_range(s->mark_us, SAC_BIT_MARK)) return 0;
        uint8_t bit;
        if (in_range(s->space_us, SAC_ONE_SPACE)) {
            bit = 1u;
        } else if (in_range(s->space_us, SAC_ZERO_SPACE) || s->space_us == 0u) {
            bit = 0u;
        } else {
            return 0;
        }
        cur = (uint8_t)(cur | (uint8_t)(bit << bp));
        if (++bp == 8u) { out[bc++] = cur; cur = 0u; bp = 0u; }
    }
    return 1;
}

/* ---------------------------------------------------------------------- *
 * 공개 API
 * ---------------------------------------------------------------------- */
int ir_decode(const ir_symbol_t *syms, size_t count, ir_decoded_t *out)
{
    if (syms == NULL || out == NULL || count == 0u) return -1;

    memset(out, 0, sizeof(*out));
    out->proto = IR_PROTO_UNKNOWN;

    uint32_t hdr_mark  = syms[0].mark_us;
    uint32_t hdr_space = syms[0].space_us;

    /* ── Samsung 에어컨 ─────────────────────────────────────────────── */
    /* 두 가지 시작 형태를 모두 인식:
     *  (a) 전역 헤더(690µs + 17844µs) + 섹션들  — syms[1] 부터 디코드
     *  (b) 섹션 헤더(3086µs + 8864µs)로 바로 시작 — syms[0] 부터 디코드
     * 17.8ms 의 전역헤더 space 가 수신 gap threshold(10ms)를 넘으면 캡처가
     * 프레임을 분할해 (b) 형태(섹션헤더로 시작)로 들어온다. */
    {
        bool global_hdr  = in_range(hdr_mark, 690u)  && in_range(hdr_space, 17844u);
        bool section_hdr = in_range(hdr_mark, SAC_SEC_MARK) &&
                           in_range(hdr_space, SAC_SEC_SPACE);
        if (global_hdr || section_hdr) {
            const ir_symbol_t *s = global_hdr ? &syms[1] : &syms[0];
            size_t             c = global_hdr ? (count - 1u) : count;
            out->proto      = IR_PROTO_SAMSUNG_AC;
            out->byte_count = decode_samsung_ac_bytes(
                                  s, c, out->raw_bytes, IR_DECODE_MAX_RAW_BYTES);
            return (out->byte_count > 0u) ? 0 : -2;
        }
    }

    /* ── LG 에어컨 (28-bit, 헤더 8.5 ms + 4.25 ms) ──────────────── */
    if (in_range(hdr_mark, 8500u) && in_range(hdr_space, 4250u) && count >= 29u) {
        uint32_t bits  = 0u;
        uint8_t  nbits = decode_space_bits(syms, count,
                                            560u, 560u, 1600u, 28u, &bits);
        if (nbits == 28u) {
            uint8_t addr    = (uint8_t)( bits        & 0xFFu);
            uint8_t cmd     = (uint8_t)((bits >>  8) & 0xFFu);
            uint8_t cmd_inv = (uint8_t)((bits >> 16) & 0xFFu);

            if ((cmd ^ cmd_inv) == 0xFFu) {
                out->proto     = IR_PROTO_LG_AC;
                out->address   = addr;
                out->command   = cmd;
                out->raw_bits  = bits;
                out->bit_count = nbits;
                return 0;
            }
        }
    }

    /* ── NEC 계열 (헤더 9 ms) ────────────────────────────────────── */
    if (in_range(hdr_mark, 9000u)) {

        /* NEC 홀드키 반복: 9 ms + 2.25 ms + 562 µs 마크 */
        if (in_range(hdr_space, 2250u)) {
            out->proto  = IR_PROTO_NEC_REPEAT;
            out->repeat = true;
            return 0;
        }

        /* NEC 풀 프레임: 9 ms + 4.5 ms + 32 비트 */
        if (in_range(hdr_space, 4500u) && count >= 33u) {
            uint32_t bits  = 0u;
            uint8_t  nbits = decode_space_bits(syms, count,
                                                562u, 562u, 1687u, 32u, &bits);
            if (nbits == 32u) {
                uint8_t addr_lo  = (uint8_t)( bits        & 0xFFu);
                uint8_t addr_hi  = (uint8_t)((bits >>  8) & 0xFFu);
                uint8_t cmd      = (uint8_t)((bits >> 16) & 0xFFu);
                uint8_t cmd_inv  = (uint8_t)((bits >> 24) & 0xFFu);

                if ((cmd ^ cmd_inv) != 0xFFu) return -2;

                out->raw_bits  = bits;
                out->bit_count = nbits;
                out->command   = cmd;

                if ((uint8_t)(addr_lo ^ addr_hi) == 0xFFu) {
                    /* 표준 NEC: addr 8-bit + 보수 확인 */
                    out->proto   = IR_PROTO_NEC;
                    out->address = addr_lo;
                } else {
                    /* NEC Extended: 16-bit 어드레스 */
                    out->proto   = IR_PROTO_NEC_EXT;
                    out->address = (uint16_t)((addr_hi << 8u) | addr_lo);
                }
                return 0;
            }
        }
    }

    return -2;   /* 알 수 없는 프로토콜 */
}

/* ---------------------------------------------------------------------- */
const char *ir_proto_name(ir_proto_t proto)
{
    switch (proto) {
    case IR_PROTO_NEC:          return "NEC";
    case IR_PROTO_NEC_EXT:      return "NEC_EXT";
    case IR_PROTO_NEC_REPEAT:   return "NEC_REPEAT";
    case IR_PROTO_LG_AC:        return "LG_AC";
    case IR_PROTO_SAMSUNG_AC:   return "SAMSUNG_AC";
    default:                    return "UNKNOWN";
    }
}
