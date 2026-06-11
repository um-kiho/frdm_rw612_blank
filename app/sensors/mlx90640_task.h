/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * MLX90640 polling task. Captures one full 32x24 image (both sub-pages) every
 * period_ms, computes per-pixel object temperature, and reports it through the
 * registered callback. A cached copy is available via
 * mlx_task_get_last_frame().
 *
 * Shares the I2C2 master with the other sensors; serialisation is provided by
 * app/i2c_bus.{h,c}. Like the AMG8833 task, if the device is not found at
 * probe time the task stops itself rather than spamming the bus with failed
 * transfers (which can stall the system during WiFi/BLE critical sections).
 */

#ifndef MLX90640_TASK_H
#define MLX90640_TASK_H

#include <stdint.h>
#include <stdbool.h>

#include "mlx90640.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mlx_sample {
    float    to[MLX90640_PIXELS];   /* object temp degC, row-major 32x24      */
    float    ta;                    /* ambient (sensor) temp degC             */
    float    min;                   /* min / max / avg over the 768 pixels    */
    float    max;
    float    avg;
    int8_t   err;                   /* 0 on success, else MLX90640_ERR_*       */
    uint16_t seq;
    uint32_t timestamp_ms;
} mlx_sample_t;

typedef void (*mlx_sample_cb_t)(const mlx_sample_t *s, void *user_arg);

/* refresh_rate: MLX90640_RR_* (per sub-page). A full image = 2 sub-pages. */
int  mlx_task_start         (uint8_t addr_7b, uint8_t refresh_rate,
                             uint32_t period_ms,
                             mlx_sample_cb_t cb, void *user_arg);
void mlx_task_stop          (void);
bool mlx_task_is_running    (void);

/* Returns 0 on success; -1 if no frame is available yet. */
int  mlx_task_get_last_frame(mlx_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MLX90640_TASK_H */
