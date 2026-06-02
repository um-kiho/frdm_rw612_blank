/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Dual-channel BH1750FVI sampling task.
 *
 * Hardware (per develop/frdmrw612_io_interface_spec.md §3):
 *   - bus  : I2C2 (FLEXCOMM2), 100 kHz bring-up
 *   - chan : BH1750 #1 at 7-bit addr 0x23 (ADDR pin = LOW)
 *            BH1750 #2 at 7-bit addr 0x5C (ADDR pin = HIGH)
 *
 * The task brings both sensors into Continuous H-resolution mode, then on
 * each tick reads both channels and reports a single combined sample via
 * the registered callback. The most recent sample is also cached and
 * accessible via lux_task_get_last().
 */

#ifndef LUX_TASK_H
#define LUX_TASK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lux_sample {
    uint32_t lux1_x100;     /* 0x23 channel, in 0.01 lx units            */
    uint32_t lux2_x100;     /* 0x5C channel, in 0.01 lx units            */
    int8_t   err1;          /* 0 on success, otherwise BH1750_ERR_*      */
    int8_t   err2;
    uint16_t seq;           /* monotonic sample sequence number          */
    uint32_t timestamp_ms;  /* xTaskGetTickCount() * portTICK_PERIOD_MS  */
} lux_sample_t;

typedef void (*lux_sample_cb_t)(const lux_sample_t *s, void *user_arg);

/* Period must be >= 200 ms when using H-resolution (typical conversion is
 * 120 ms per channel, total ~240 ms for two back-to-back reads). */
int  lux_task_start   (uint32_t period_ms,
                       lux_sample_cb_t cb, void *user_arg);

void lux_task_stop    (void);
bool lux_task_is_running(void);

/* Returns 0 on success; -1 if no sample has been captured yet. */
int  lux_task_get_last(lux_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LUX_TASK_H */
