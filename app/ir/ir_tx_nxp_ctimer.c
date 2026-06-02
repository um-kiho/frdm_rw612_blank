/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * NXP CTimer + SCTimer backend implementing ir_tx.h.
 *
 * Sequencer state machine:
 *
 *   IDLE -- ir_tx_send_symbols() --> ARMED
 *
 *   ARMED:
 *     idx = 0; phase = MARK
 *     carrier_on()                            (route pin to SCT output)
 *     CTimer.MR0 = current_count + mark_us[0]
 *     CTimer.start()
 *
 *   MARK end ISR:
 *     carrier_off()                           (route pin to plain GPIO=0)
 *     CTimer.MR0 += space_us[idx]
 *     phase = SPACE
 *
 *   SPACE end ISR:
 *     idx++
 *     if idx == count:
 *         CTimer.stop(); give(done_sem); return
 *     carrier_on()
 *     CTimer.MR0 += mark_us[idx]
 *     phase = MARK
 *
 * The CTimer base count is free-running so we accumulate match values
 * instead of resetting per symbol; this eliminates the half-cycle jitter
 * that resets would introduce.
 *
 * Because the actual NXP SDK calls (CTIMER_*, SCTIMER_*, IOPCTL_*) depend
 * on the exact RW612 SDK revision picked up by reconfig.cmake, the
 * register-level bodies below are gated by APP_IR_TX_NXP_REAL_HW. When
 * the macro is 0 (default until you wire the SDK in pin_mux.c) the file
 * still compiles and produces a "would-have-sent N symbols" log line so
 * the upper layers (encoder, BLE service) can be exercised end-to-end.
 *
 * To enable the real hardware path:
 *   1. Confirm CTimer instance + IRQ in MCUXpresso config tools.
 *   2. Confirm the SCTimer output number + carrier pin alt function in
 *      pin_mux.c (APP_IR_TX_FUNC_SCT vs APP_IR_TX_FUNC_GPIO).
 *   3. Define APP_IR_TX_NXP_REAL_HW=1 in reconfig.cmake mcux_add_macro.
 */

#include "ir_tx_nxp_ctimer.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifndef APP_IR_TX_NXP_REAL_HW
#define APP_IR_TX_NXP_REAL_HW   0
#endif

LOG_MODULE_REGISTER(app_ir_tx, LOG_LEVEL_INF);

#if (APP_IR_TX_NXP_REAL_HW != 0)
#include "fsl_common.h"
#include "fsl_ctimer.h"
#include "fsl_sctimer.h"
#include "fsl_iopctl.h"
#include "fsl_clock.h"
#endif

/* ---------------------------------------------------------------------- *
 * Hardware-specific helper macros
 * ---------------------------------------------------------------------- */
#if (APP_IR_TX_NXP_REAL_HW != 0)

#if   (APP_IR_TX_CTIMER_INSTANCE == 0u)
  #define APP_IR_CTIMER_BASE   CTIMER0
  #define APP_IR_CTIMER_IRQN   CTIMER0_IRQn
#elif (APP_IR_TX_CTIMER_INSTANCE == 1u)
  #define APP_IR_CTIMER_BASE   CTIMER1
  #define APP_IR_CTIMER_IRQN   CTIMER1_IRQn
#elif (APP_IR_TX_CTIMER_INSTANCE == 2u)
  #define APP_IR_CTIMER_BASE   CTIMER2
  #define APP_IR_CTIMER_IRQN   CTIMER2_IRQn
#elif (APP_IR_TX_CTIMER_INSTANCE == 3u)
  #define APP_IR_CTIMER_BASE   CTIMER3
  #define APP_IR_CTIMER_IRQN   CTIMER3_IRQn
#else
  #error "APP_IR_TX_CTIMER_INSTANCE must be 0..3"
#endif

#define APP_IR_CTIMER_MR0      kCTIMER_Match_0

#endif /* APP_IR_TX_NXP_REAL_HW */

/* ---------------------------------------------------------------------- *
 * Module state
 * ---------------------------------------------------------------------- */
typedef enum {
    PHASE_IDLE = 0,
    PHASE_MARK,
    PHASE_SPACE,
} phase_t;

static volatile phase_t       s_phase;
static volatile size_t        s_idx;
static volatile size_t        s_count;
static const ir_symbol_t     *s_syms;

static struct k_sem           s_done_sem;
static struct k_mutex         s_busy_mtx;
static volatile bool          s_inited;
static volatile uint32_t      s_carrier_hz = APP_IR_TX_DEFAULT_CARRIER_HZ;

/* ---------------------------------------------------------------------- *
 * Pin gating: carrier_on() routes the IR-LED pin to the SCTimer output
 * (38 kHz square wave). carrier_off() routes it to plain GPIO held low.
 * The two functions are the *only* place that touches the IOPCTL mux at
 * run time, so they stay tight.
 * ---------------------------------------------------------------------- */
static inline void carrier_on(void)
{
#if (APP_IR_TX_NXP_REAL_HW != 0)
    IOPCTL_PinMuxSet(IOPCTL,
                     APP_IR_TX_GPIO_PORT, APP_IR_TX_GPIO_PIN,
                     APP_IR_TX_FUNC_SCT |
                     IOPCTL_PIO_PUPD_DI |
                     IOPCTL_PIO_INBUF_DI |
                     IOPCTL_PIO_SLEW_RATE_FAST |
                     IOPCTL_PIO_FULLDRIVE_EN |
                     IOPCTL_PIO_INV_DI);
#endif
}

static inline void carrier_off(void)
{
#if (APP_IR_TX_NXP_REAL_HW != 0)
    IOPCTL_PinMuxSet(IOPCTL,
                     APP_IR_TX_GPIO_PORT, APP_IR_TX_GPIO_PIN,
                     APP_IR_TX_FUNC_GPIO |
                     IOPCTL_PIO_PUPD_DI |
                     IOPCTL_PIO_INBUF_DI |
                     IOPCTL_PIO_SLEW_RATE_NORMAL |
                     IOPCTL_PIO_FULLDRIVE_EN |
                     IOPCTL_PIO_INV_DI);
    /* Pin function was just swapped to plain GPIO; the GPIO output reg
     * is left at 0 by ir_tx_init() so the IR LED is off here. */
#endif
}

/* ---------------------------------------------------------------------- *
 * CTimer match ISR
 * ---------------------------------------------------------------------- */
#if (APP_IR_TX_NXP_REAL_HW != 0)
static void ctimer_match_cb(uint32_t flags)
{
    (void)flags;

    if (s_phase == PHASE_MARK) {
        /* Mark just ended -> start the trailing space. */
        carrier_off();
        uint16_t sp = s_syms[s_idx].space_us;
        uint32_t cur = CTIMER_GetCurrentTimerCount(APP_IR_CTIMER_BASE);
        CTIMER_SetupMatch(APP_IR_CTIMER_BASE, APP_IR_CTIMER_MR0,
                          /* match_value = */ cur + (uint32_t)sp);
        s_phase = PHASE_SPACE;
        return;
    }

    /* SPACE just ended -> advance to next symbol or finish. */
    s_idx++;
    if (s_idx >= s_count) {
        CTIMER_StopTimer(APP_IR_CTIMER_BASE);
        carrier_off();                       /* defensive: pin must idle low */
        s_phase = PHASE_IDLE;
        k_sem_give(&s_done_sem);
        return;
    }

    uint16_t mk = s_syms[s_idx].mark_us;
    uint32_t cur = CTIMER_GetCurrentTimerCount(APP_IR_CTIMER_BASE);
    carrier_on();
    CTIMER_SetupMatch(APP_IR_CTIMER_BASE, APP_IR_CTIMER_MR0,
                      cur + (uint32_t)mk);
    s_phase = PHASE_MARK;
}
#endif /* APP_IR_TX_NXP_REAL_HW */

/* ---------------------------------------------------------------------- *
 * Carrier generator: SCTimer 38 kHz, 33 % duty
 * ---------------------------------------------------------------------- */
static int carrier_init(uint32_t hz)
{
#if (APP_IR_TX_NXP_REAL_HW != 0)
    sctimer_config_t sctcfg;
    SCTIMER_GetDefaultConfig(&sctcfg);
    sctcfg.clockMode = kSCTIMER_System_ClockMode;

    if (SCTIMER_Init(SCT0, &sctcfg) != kStatus_Success) return -1;

    sctimer_pwm_signal_param_t pwm;
    pwm.output           = APP_IR_TX_SCT_CARRIER_OUTPUT;
    pwm.level            = kSCTIMER_HighTrue;
    pwm.dutyCyclePercent = 33u;

    uint32_t event;
    if (SCTIMER_SetupPwm(SCT0, &pwm, kSCTIMER_CenterAlignedPwm,
                         hz, CLOCK_GetFreq(kCLOCK_BusClk), &event)
        != kStatus_Success) {
        return -2;
    }
    SCTIMER_StartTimer(SCT0, kSCTIMER_Counter_U);
    s_carrier_hz = hz;
    return 0;
#else
    s_carrier_hz = hz;
    return 0;
#endif
}

/* ---------------------------------------------------------------------- *
 * Public API (ir_tx.h)
 * ---------------------------------------------------------------------- */
int ir_tx_init(void)
{
    if (s_inited) return 0;

    k_sem_init(&s_done_sem, 0, 1);
    k_mutex_init(&s_busy_mtx);

    if (carrier_init(APP_IR_TX_DEFAULT_CARRIER_HZ) != 0) return -2;

#if (APP_IR_TX_NXP_REAL_HW != 0)
    /* 1 MHz tick = 1 us per CTimer count. */
    ctimer_config_t ctcfg;
    CTIMER_GetDefaultConfig(&ctcfg);
    uint32_t src = CLOCK_GetFreq(kCLOCK_BusClk);
    ctcfg.prescale = (src / 1000000u) - 1u;
    CTIMER_Init(APP_IR_CTIMER_BASE, &ctcfg);

    ctimer_match_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.enableCounterReset = false;
    mcfg.enableCounterStop  = false;
    mcfg.matchValue         = 0xFFFFFFFFu;       /* loaded per symbol  */
    mcfg.outControl         = kCTIMER_Output_NoAction;
    mcfg.enableInterrupt    = true;
    CTIMER_SetupMatch(APP_IR_CTIMER_BASE, APP_IR_CTIMER_MR0, mcfg.matchValue);

    static ctimer_callback_t cbs[] = { ctimer_match_cb, NULL, NULL, NULL };
    CTIMER_RegisterCallBack(APP_IR_CTIMER_BASE, cbs, kCTIMER_SingleCallback);

    NVIC_SetPriority(APP_IR_CTIMER_IRQN, configMAX_SYSCALL_INTERRUPT_PRIORITY);
    EnableIRQ(APP_IR_CTIMER_IRQN);
#endif

    carrier_off();
    s_phase   = PHASE_IDLE;
    s_inited  = true;
    LOG_INF("ready (carrier=%u Hz, hw=%d)",
            (unsigned)s_carrier_hz, (int)APP_IR_TX_NXP_REAL_HW);
    return 0;
}

void ir_tx_deinit(void)
{
#if (APP_IR_TX_NXP_REAL_HW != 0)
    if (s_inited) {
        DisableIRQ(APP_IR_CTIMER_IRQN);
        CTIMER_Deinit(APP_IR_CTIMER_BASE);
        SCTIMER_Deinit(SCT0);
    }
#endif
    s_inited = false;
}

bool ir_tx_is_ready(void)
{
    return s_inited;
}

int ir_tx_set_carrier_hz(uint32_t hz)
{
    if (hz < 25000u || hz > 60000u) return -1;
    if (!s_inited) return -2;
    /* Tear down and re-init the carrier; it's a one-shot register write
     * on SCTimer once configured. */
    return carrier_init(hz);
}

uint32_t ir_tx_get_carrier_hz(void)
{
    return s_carrier_hz;
}

int ir_tx_send_symbols(const ir_symbol_t *syms, size_t count)
{
    if (!s_inited) return -1;
    if (syms == NULL || count == 0u) return -2;
    if (count > APP_IR_TX_MAX_SYMBOLS) return -3;

    k_mutex_lock(&s_busy_mtx, K_FOREVER);

    /* Drain any leftover give() from a previous interrupted send. */
    (void)k_sem_take(&s_done_sem, K_NO_WAIT);

    s_syms  = syms;
    s_count = count;
    s_idx   = 0;
    s_phase = PHASE_MARK;

#if (APP_IR_TX_NXP_REAL_HW != 0)
    /* Arm CTimer at (current + first mark) and let the ISR drive the rest. */
    CTIMER_Reset(APP_IR_CTIMER_BASE);
    CTIMER_SetupMatch(APP_IR_CTIMER_BASE, APP_IR_CTIMER_MR0,
                      (uint32_t)syms[0].mark_us);
    carrier_on();
    CTIMER_StartTimer(APP_IR_CTIMER_BASE);
#else
    /* Stub: log the frame so the upper layers can be exercised before
     * the hardware bring-up is finished. */
    LOG_INF("STUB send %u symbols (first mk=%u sp=%u)",
            (unsigned)count,
            (unsigned)syms[0].mark_us, (unsigned)syms[0].space_us);
    k_sem_give(&s_done_sem);
#endif

    int ret = (k_sem_take(&s_done_sem, K_MSEC(APP_IR_TX_SEND_TIMEOUT_MS)) == 0) ? 0 : -5;

#if (APP_IR_TX_NXP_REAL_HW != 0)
    if (ret != 0) {
        /* Timeout: stop the sequencer to avoid leaking. */
        CTIMER_StopTimer(APP_IR_CTIMER_BASE);
        carrier_off();
        s_phase = PHASE_IDLE;
    }
#endif

    k_mutex_unlock(&s_busy_mtx);
    return ret;
}
