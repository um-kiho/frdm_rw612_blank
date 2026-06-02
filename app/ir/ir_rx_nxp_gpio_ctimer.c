/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * IR receive backend — GPIO18 (HSGPIO0[18], DT alias "ir-rx"), TSDP341.
 *
 * ── TSDP341 signal polarity ───────────────────────────────────────────────
 *   GPIO LOW  (TSDP341 OUT active) = IR mark  (38 kHz carrier detected)
 *   GPIO HIGH (TSDP341 OUT idle)   = IR space (no carrier)
 *   → GPIO_ACTIVE_LOW in device tree, so gpio_pin_get_dt() returns 1 = mark.
 *
 * ── Edge → symbol assembly ────────────────────────────────────────────────
 *   Falling edge on the raw GPIO pin (= active level starting):
 *     mark begins → record timestamp in s_last_cyc
 *
 *   Rising edge on the raw GPIO pin (= active level ending):
 *     mark just ended → mark_us = cycles_to_us(now − s_last_cyc)
 *     push {mark_us, 0} into buffer; patch space_us on the next fall.
 *
 *   Next falling edge:
 *     space_us = cycles_to_us(now − s_last_cyc)
 *     write into buf[len-1].space_us → symbol complete.
 *
 *   Gap timer fires after APP_IR_RX_GAP_WORK_MS of silence:
 *     last symbol has space_us = 0 (gap, not a measured space — by spec).
 *     Swap double buffer, invoke user callback.
 *
 * ── Double-buffer ─────────────────────────────────────────────────────────
 *   The ISR callback writes to s_buf[s_wr_idx].
 *   Gap work atomically flips s_wr_idx and delivers the completed frame.
 *   The callback reads the old half while the ISR fills the new half.
 *
 * ── Timestamp precision ───────────────────────────────────────────────────
 *   k_cycle_get_32() at 200 MHz gives 5 ns resolution.
 *   ISR latency jitter ≈ 1–3 µs — well within IR timing tolerance (±25 %).
 *   Wraparound at 200 MHz: 2^32 / 200e6 ≈ 21 s — IR frames are < 200 ms.
 */

#include "ir_rx_nxp_gpio_ctimer.h"

#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_ir_rx, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------- *
 * DT GPIO spec  (hsgpio0 pin 18, active-low, pull-up — frdm_rw612.overlay)
 *
 * Alias "ir-rx" → &ir_rx_pin → gpios = <&hsgpio0 18 (GPIO_ACTIVE_LOW|...)>
 * DT_ALIAS(ir_rx) must be a literal token; do NOT pass through a #define.
 * ---------------------------------------------------------------------- */
#if !DT_NODE_EXISTS(DT_ALIAS(ir_rx))
#error "DT alias 'ir-rx' not found. " \
       "Check ir_rx_pin node in boards/frdm_rw612.overlay."
#endif

static const struct gpio_dt_spec s_ir_rx =
    GPIO_DT_SPEC_GET(DT_ALIAS(ir_rx), gpios);

static struct gpio_callback s_gpio_cb;

/* ---------------------------------------------------------------------- *
 * Timestamp helpers (k_cycle_get_32 → µs)
 * ---------------------------------------------------------------------- */
static uint32_t s_cyc_per_us;   /* pre-computed in ir_rx_init() */

static inline uint32_t cyc_to_us(uint32_t cycles)
{
    /* Integer division; at 200 MHz s_cyc_per_us = 200, so 1 count = 5 ns.
     * clamped to 16-bit max when stored in ir_symbol_t. */
    return (s_cyc_per_us > 0u) ? (cycles / s_cyc_per_us) : 0u;
}

/* ---------------------------------------------------------------------- *
 * Double frame buffer
 * ---------------------------------------------------------------------- */
static ir_symbol_t s_buf[2][APP_IR_RX_MAX_SYMBOLS];

static volatile uint8_t  s_wr_idx;   /* which buffer the ISR fills  */
static volatile size_t   s_wr_len;   /* symbols written so far       */

/* ---------------------------------------------------------------------- *
 * ISR edge state
 * ---------------------------------------------------------------------- */
typedef enum { RX_IDLE = 0, RX_MARK, RX_SPACE } rx_phase_t;

static volatile rx_phase_t  s_phase;
static volatile uint32_t    s_last_cyc;   /* k_cycle_get_32() at last edge */

/* ---------------------------------------------------------------------- *
 * Module state
 * ---------------------------------------------------------------------- */
static ir_rx_callback_t  s_cb;
static void             *s_cb_user;
static volatile bool     s_rx_active;
static bool              s_inited;

/* ---------------------------------------------------------------------- *
 * Gap work
 * ---------------------------------------------------------------------- */
static struct k_work_delayable s_gap_work;

static void gap_work_handler(struct k_work *work)
{
    (void)work;

    /* Atomically flip the write buffer and snapshot the frame length. */
    unsigned int key = irq_lock();
    uint8_t done_idx  = s_wr_idx;
    size_t  done_len  = s_wr_len;
    s_wr_idx  = (uint8_t)(done_idx ^ 1u);
    s_wr_len  = 0u;
    s_phase   = RX_IDLE;
    irq_unlock(key);

    if (done_len >= APP_IR_RX_MIN_SYMBOLS && s_cb != NULL) {
        s_cb(s_buf[done_idx], done_len, s_cb_user);
    }
}

/* ---------------------------------------------------------------------- *
 * GPIO interrupt callback (Zephyr GPIO API, fires in ISR context)
 * ---------------------------------------------------------------------- */
static void gpio_edge_cb(const struct device *dev,
                         struct gpio_callback *cb,
                         uint32_t pins)
{
    (void)cb;
    (void)pins;

    if (!s_rx_active) {
        return;
    }

    uint32_t now     = k_cycle_get_32();
    uint32_t dt_cyc  = now - s_last_cyc;   /* handles 32-bit wraparound */
    s_last_cyc       = now;

    /* Read the raw (physical) pin level.
     * gpio_pin_get_raw() returns 0=LOW (mark active) or 1=HIGH (space/idle).
     * We use raw level because ACTIVE_LOW inversion would flip the meaning. */
    int raw_level = gpio_pin_get_raw(dev, s_ir_rx.pin);

    /* Re-arm the gap timer on every edge. */
    k_work_reschedule(&s_gap_work, K_MSEC(APP_IR_RX_GAP_WORK_MS));

    uint16_t dur_us = (uint16_t)MIN(cyc_to_us(dt_cyc), (uint32_t)0xFFFFu);

    if (raw_level == 0) {
        /* Falling edge: GPIO went LOW → mark is starting. */
        if (s_phase == RX_IDLE) {
            /* First mark of this frame; just record the start time. */
            s_phase = RX_MARK;
            return;
        }
        if (s_phase == RX_SPACE) {
            /* Space just ended: patch previous symbol's space_us. */
            size_t idx = s_wr_len;
            if (idx > 0u) {
                s_buf[s_wr_idx][idx - 1u].space_us = dur_us;
            }
            s_phase = RX_MARK;
        }
    } else {
        /* Rising edge: GPIO went HIGH → mark just ended. */
        if (s_phase == RX_MARK) {
            size_t idx = s_wr_len;
            if (idx < APP_IR_RX_MAX_SYMBOLS) {
                s_buf[s_wr_idx][idx].mark_us  = dur_us;
                s_buf[s_wr_idx][idx].space_us = 0u;   /* filled on next fall */
                s_wr_len = idx + 1u;
            }
            s_phase = RX_SPACE;
        }
    }
}

/* ---------------------------------------------------------------------- *
 * Public API (ir_rx.h)
 * ---------------------------------------------------------------------- */
int ir_rx_init(void)
{
    if (s_inited) {
        return 0;
    }

    /* Pre-compute cycles-per-microsecond once. */
    s_cyc_per_us = (uint32_t)(sys_clock_hw_cycles_per_sec() / 1000000u);
    if (s_cyc_per_us == 0u) {
        s_cyc_per_us = 1u;   /* safety; should never happen */
    }

    if (!gpio_is_ready_dt(&s_ir_rx)) {
        LOG_ERR("IR RX GPIO device not ready (alias ir-rx / GPIO18)");
        return -ENODEV;
    }

    int rc = gpio_pin_configure_dt(&s_ir_rx, GPIO_INPUT);
    if (rc != 0) {
        LOG_ERR("gpio_pin_configure_dt failed: %d", rc);
        return rc;
    }

    gpio_init_callback(&s_gpio_cb, gpio_edge_cb, BIT(s_ir_rx.pin));
    rc = gpio_add_callback(s_ir_rx.port, &s_gpio_cb);
    if (rc != 0) {
        LOG_ERR("gpio_add_callback failed: %d", rc);
        return rc;
    }

    /* Leave interrupt disabled until ir_rx_start() is called. */

    k_work_init_delayable(&s_gap_work, gap_work_handler);

    s_wr_idx     = 0u;
    s_wr_len     = 0u;
    s_phase      = RX_IDLE;
    s_last_cyc   = k_cycle_get_32();
    s_rx_active  = false;
    s_inited     = true;

    LOG_INF("ready (GPIO%u, cyc/us=%u)",
            (unsigned)s_ir_rx.pin, (unsigned)s_cyc_per_us);
    return 0;
}

void ir_rx_deinit(void)
{
    if (!s_inited) {
        return;
    }
    gpio_pin_interrupt_configure_dt(&s_ir_rx, GPIO_INT_DISABLE);
    gpio_remove_callback(s_ir_rx.port, &s_gpio_cb);
    k_work_cancel_delayable(&s_gap_work);
    s_rx_active = false;
    s_inited    = false;
}

void ir_rx_set_callback(ir_rx_callback_t cb, void *user)
{
    s_cb      = cb;
    s_cb_user = user;
}

int ir_rx_start(void)
{
    if (!s_inited) {
        int rc = ir_rx_init();
        if (rc != 0) {
            return rc;
        }
    }

    unsigned int key = irq_lock();
    s_wr_len    = 0u;
    s_phase     = RX_IDLE;
    s_last_cyc  = k_cycle_get_32();
    s_rx_active = true;
    irq_unlock(key);

    int rc = gpio_pin_interrupt_configure_dt(&s_ir_rx, GPIO_INT_EDGE_BOTH);
    if (rc != 0) {
        LOG_ERR("gpio_pin_interrupt_configure_dt failed: %d", rc);
        s_rx_active = false;
        return rc;
    }

    LOG_INF("capture started (GPIO%u)", (unsigned)s_ir_rx.pin);
    return 0;
}

void ir_rx_stop(void)
{
    s_rx_active = false;
    gpio_pin_interrupt_configure_dt(&s_ir_rx, GPIO_INT_DISABLE);
    k_work_cancel_delayable(&s_gap_work);
    LOG_INF("capture stopped");
}

bool ir_rx_is_active(void)
{
    return s_rx_active;
}

void ir_rx_inject_frame(const ir_symbol_t *syms, size_t count)
{
    if (syms == NULL || count == 0u) {
        return;
    }
    if (count > APP_IR_RX_MAX_SYMBOLS) {
        count = APP_IR_RX_MAX_SYMBOLS;
    }

    unsigned int key = irq_lock();
    memcpy(s_buf[s_wr_idx], syms, count * sizeof(ir_symbol_t));
    size_t buf_idx = s_wr_idx;
    irq_unlock(key);

    LOG_INF("inject %u symbols", (unsigned)count);

    if (count >= APP_IR_RX_MIN_SYMBOLS && s_cb != NULL) {
        s_cb(s_buf[buf_idx], count, s_cb_user);
    }
}
