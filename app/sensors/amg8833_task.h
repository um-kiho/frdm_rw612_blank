/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * AMG8833 polling task. Captures one 8x8 frame every period_ms and reports
 * it through the registered callback. Also keeps a cached copy that callers
 * may grab with amg_task_get_last_frame() to avoid copying through the
 * callback path.
 *
 * The task and lux_task share the same I2C2 master; serialisation is
 * provided by app/i2c_bus.{h,c}.
 */

#ifndef AMG_TASK_H
#define AMG_TASK_H

#include <stdint.h>
#include <stdbool.h>

#include "amg8833.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct amg_sample {
    int16_t  pixels_q2[AMG8833_PIXELS]; /* 0.25 C/LSB                       */
    int16_t  thermistor_q4;             /* 0.0625 C/LSB                     */
    int16_t  min_q2;                    /* hot-pixel min over the 64 px     */
    int16_t  max_q2;                    /* hot-pixel max                    */
    int16_t  avg_q2;                    /* average                          */
    int8_t   err;                       /* 0 on success, else AMG8833_ERR_* */
    uint16_t seq;
    uint32_t timestamp_ms;
} amg_sample_t;

typedef void (*amg_sample_cb_t)(const amg_sample_t *s, void *user_arg);

int  amg_task_start         (uint8_t addr_7b, uint32_t period_ms,
                             amg_sample_cb_t cb, void *user_arg);
void amg_task_stop          (void);
bool amg_task_is_running    (void);

/* Returns 0 on success; -1 if no frame is available yet. */
int  amg_task_get_last_frame(amg_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AMG_TASK_H */
