/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mlx90640_task.h"
#include "i2c_bus.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mlx_task, LOG_LEVEL_INF);

#ifndef MLX_TASK_STACK
#define MLX_TASK_STACK          6144   /* init/extract + 512B I2C rx + float frames */
#endif
#ifndef MLX_TASK_PRIO
#define MLX_TASK_PRIO           2
#endif
#ifndef MLX_TASK_MIN_PERIOD_MS
#define MLX_TASK_MIN_PERIOD_MS  250u
#endif

/* Emissivity / reflected-temperature defaults for the To computation. */
#ifndef MLX_EMISSIVITY
#define MLX_EMISSIVITY          0.95f
#endif

static mlx90640_t         s_dev;          /* ~11 KB — keep out of the stack    */
static K_MUTEX_DEFINE(s_lock);
static volatile bool      s_running;
static volatile bool      s_has_frame;
static mlx_sample_t       s_last;         /* ~3 KB cached frame                */
static mlx_sample_cb_t    s_cb;
static void              *s_cb_arg;
static uint32_t           s_period_ms;
static uint8_t            s_addr;
static uint8_t            s_rr;
static k_tid_t            s_tid;

/* Compact 32x24 grid print, integer-formatted (avoids %f / FP printf support).
 * Disabled by default — uncomment the call site to dump the full image. */
__attribute__((unused))
static void mlx_print_grid(const float *to)
{
    printf("MLX90640\n");
    for (int r = 0; r < MLX90640_ROWS; ++r) {
        for (int c = 0; c < MLX90640_COLS; ++c) {
            int t10 = (int)lroundf(to[r * MLX90640_COLS + c] * 10.0f);
            printf("%4d.%d", t10 / 10, (t10 < 0 ? -t10 : t10) % 10);
        }
        printf("$end\n");
    }
}

static void compute_stats(mlx_sample_t *s)
{
    float mn = s->to[0], mx = s->to[0];
    double sum = 0.0;
    for (int i = 0; i < MLX90640_PIXELS; ++i) {
        float v = s->to[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    s->min = mn;
    s->max = mx;
    s->avg = (float)(sum / MLX90640_PIXELS);
}

/* Fill s_last from the current s_dev state and notify. Single-writer (this
 * task), so the callback may read s_last directly; concurrent readers go
 * through mlx_task_get_last_frame() which takes the same mutex. */
static void publish(int8_t err, uint16_t seq)
{
    k_mutex_lock(&s_lock, K_FOREVER);
    if (err == 0) {
        memcpy(s_last.to, s_dev.To, sizeof(s_last.to));
        s_last.ta = mlx90640_get_ta(&s_dev);
        compute_stats(&s_last);
    }
    s_last.err          = err;
    s_last.seq          = seq;
    s_last.timestamp_ms = k_uptime_get_32();
    s_has_frame         = (err == 0) || s_has_frame;
    k_mutex_unlock(&s_lock);

    if (s_cb != NULL) {
        s_cb(&s_last, s_cb_arg);
    }
}

static void mlx_task(void *p1, void *p2, void *p3)
{
    (void)p1; (void)p2; (void)p3;
    uint16_t seq = 0u;

    k_sleep(K_MSEC(300));
    LOG_INF("probing I2C addr=0x%02X ...", s_addr);

    bool found = (i2c_bus_probe(s_addr) == 0);
    int  rc    = found ? mlx90640_init(&s_dev, s_addr, s_rr) : MLX90640_ERR_IO;

    if (!found || rc != MLX90640_OK) {
        /* Same policy as the AMG task: do not keep polling a missing sensor —
         * failed I2C transfers colliding with WiFi/BLE can stall the system.
         * Connect the sensor and reboot to re-detect. */
        LOG_WRN("MLX90640 not found (addr=0x%02X rc=%d) — stopping (no I2C retry)",
                s_addr, rc);
        s_running = false;
        s_tid     = NULL;
        return;
    }
    LOG_INF("MLX90640 ready (addr=0x%02X)", s_addr);

    uint32_t last_print = 0u;
    uint32_t next_time  = k_uptime_get_32();

    while (s_running) {
        int8_t err = 0;

        /* A full image needs both sub-pages. get_frame() blocks on data-ready,
         * so this naturally paces to the sensor's refresh rate. */
        for (int sp = 0; sp < 2 && s_running; ++sp) {
            int r = mlx90640_get_frame(&s_dev);
            if (r < 0) { err = (int8_t)r; break; }
            mlx90640_calculate_to(&s_dev, MLX_EMISSIVITY,
                                  mlx90640_get_ta(&s_dev) - 8.0f);
        }

        publish(err, ++seq);

        if (err == 0) {
            uint32_t now_ms = k_uptime_get_32();
            if ((now_ms - last_print) >= 1000u) {
                last_print = now_ms;
                int ta10  = (int)lroundf(s_last.ta  * 10.0f);
                int mn10  = (int)lroundf(s_last.min * 10.0f);
                int mx10  = (int)lroundf(s_last.max * 10.0f);
                int av10  = (int)lroundf(s_last.avg * 10.0f);
                printf("MLX90640 32x24 (C)  Ta=%d.%d  min=%d.%d max=%d.%d avg=%d.%d\n",
                       ta10 / 10, (ta10 < 0 ? -ta10 : ta10) % 10,
                       mn10 / 10, (mn10 < 0 ? -mn10 : mn10) % 10,
                       mx10 / 10, (mx10 < 0 ? -mx10 : mx10) % 10,
                       av10 / 10, (av10 < 0 ? -av10 : av10) % 10);
                /* mlx_print_grid(s_last.to);  // 전체 그리드 덤프 */
            }
        } else {
            LOG_WRN("frame error rc=%d", err);
        }

        next_time += s_period_ms;
        uint32_t now = k_uptime_get_32();
        if ((int32_t)(next_time - now) < 1) {
            next_time = now + s_period_ms;   /* schedule slipped → reset */
        }
        k_sleep(K_TIMEOUT_ABS_MS(next_time));
    }

    s_tid = NULL;
}

int mlx_task_start(uint8_t addr_7b, uint8_t refresh_rate, uint32_t period_ms,
                   mlx_sample_cb_t cb, void *user_arg)
{
    if (refresh_rate > MLX90640_RR_64HZ) return -1;
    if (period_ms < MLX_TASK_MIN_PERIOD_MS) {
        period_ms = MLX_TASK_MIN_PERIOD_MS;
    }
    if (s_running) return 0;

    s_addr      = addr_7b;
    s_rr        = refresh_rate;
    s_cb        = cb;
    s_cb_arg    = user_arg;
    s_period_ms = period_ms;
    s_has_frame = false;
    s_running   = true;

    static K_THREAD_STACK_DEFINE(mlx_task_stack, MLX_TASK_STACK);
    static struct k_thread mlx_task_data;
    s_tid = k_thread_create(&mlx_task_data, mlx_task_stack,
                            K_THREAD_STACK_SIZEOF(mlx_task_stack),
                            mlx_task, NULL, NULL, NULL,
                            MLX_TASK_PRIO, 0, K_NO_WAIT);
    if (s_tid == NULL) {
        s_running = false;
        return -3;
    }
    k_thread_name_set(s_tid, "zcube_mlx");
    return 0;
}

void mlx_task_stop(void)
{
    s_running = false;
}

bool mlx_task_is_running(void)
{
    return s_running;
}

int mlx_task_get_last_frame(mlx_sample_t *out)
{
    if (out == NULL) return -1;
    if (!s_has_frame) return -1;
    k_mutex_lock(&s_lock, K_FOREVER);
    *out = s_last;
    k_mutex_unlock(&s_lock);
    return 0;
}
