/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared FlexComm I2C bus front-end.
 *
 * Every device driver on the bus (BH1750 ×2, AMG8833, ...) MUST go through
 * the i2c_bus_* API so that concurrent transfers from different tasks are
 * serialised by the internal mutex.
 *
 * Default target follows develop/frdmrw612_io_interface_spec.md §3:
 *   bus    : I2C2 (FLEXCOMM2)
 *   speed  : 400 kHz Fast-mode
 *            - BH1750FVI Fast-mode max: 400 kHz (datasheet §Electrical Char.)
 *            - AMG8833  Fast-mode max : 400 kHz (datasheet §I2C Characteristics)
 *            Override to 100 kHz from the build system if signal integrity
 *            is in doubt during bring-up.
 */

#ifndef APP_I2C_BUS_H
#define APP_I2C_BUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APP_I2C_BUS_INSTANCE
#define APP_I2C_BUS_INSTANCE       2u
#endif
#ifndef APP_I2C_BUS_BAUDRATE_HZ
#define APP_I2C_BUS_BAUDRATE_HZ    400000u
#endif

/* Initialise the FlexComm I2C master + internal mutex. Safe to call from
 * multiple drivers; subsequent calls are no-ops. Returns 0 on success. */
int i2c_bus_init      (void);

/* Raw single-transaction write / read. */
int i2c_bus_write     (uint8_t addr_7b, const uint8_t *data, size_t len);
int i2c_bus_read      (uint8_t addr_7b,       uint8_t *data, size_t len);

/* Common "write register pointer, then read N bytes with repeated start"
 * pattern. tx_len must be 1..4 (fsl_i2c subaddress is up to 32 bits). The
 * whole sequence is executed inside one mutex hold. */
int i2c_bus_write_read(uint8_t addr_7b,
                       const uint8_t *tx, size_t tx_len,
                       uint8_t       *rx, size_t rx_len);

/* 1-byte read 시도로 I2C 슬레이브 존재 여부 확인.
 * 장치가 ACK를 보내면 0, NACK(없음)이면 -ENODEV 반환. */
int  i2c_bus_probe        (uint8_t addr_7b);

/* k_thread_abort() 후 뮤텍스가 잠긴 채 남은 경우 강제 재초기화.
 * 워치독 핸들러에서만 호출. */
void i2c_bus_force_unlock (void);

#ifdef __cplusplus
}
#endif

#endif /* APP_I2C_BUS_H */
