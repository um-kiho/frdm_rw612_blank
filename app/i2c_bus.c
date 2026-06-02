/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "i2c_bus.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/device.h>

#include <fsl_clock.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2c_bus, LOG_LEVEL_INF);

#if   (APP_I2C_BUS_INSTANCE == 0u)
  #define I2C_NODE DT_NODELABEL(flexcomm0)
#elif (APP_I2C_BUS_INSTANCE == 1u)
  #define I2C_NODE DT_NODELABEL(flexcomm1)
#elif (APP_I2C_BUS_INSTANCE == 2u)
  #define I2C_NODE DT_NODELABEL(flexcomm2)
#elif (APP_I2C_BUS_INSTANCE == 3u)
  #define I2C_NODE DT_NODELABEL(flexcomm3)
#else
  #error "APP_I2C_BUS_INSTANCE must be 0..3"
#endif

/* Resolved at link time; speed/pinctrl configured in the device tree. */
static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_NODE);

/* Statically initialised — no runtime k_mutex_init() call needed. */
static K_MUTEX_DEFINE(s_lock);

/* ── RW612 FlexComm I2C 활성화 보정 (필수) ───────────────────────────────────
 * Zephyr의 mcux_flexcomm I2C 드라이버는 FLEXCOMM_Init()를 호출하지 않는다.
 * FLEXCOMM_Init()는 FlexComm 래퍼의 PSELID 레지스터에 "어느 주변장치(USART/SPI/
 * I2C)를 쓸지"를 write 하여 해당 엔진을 활성화한다. 드라이버 init 의
 * reset_line_toggle() 이 이 선택을 지운 뒤 재선택하지 않으므로, 이 RW612 에서는
 * I2C 엔진이 활성화되지 않는다 → 기능 레지스터(CFG 등)가 RAZ/WI, 모든 전송이
 * 100ms 타임아웃 (PSELID READ 는 PERSEL=I2C 로 보이지만 엔진은 죽어 있음).
 *
 * 실험으로 확인된 사실:
 *  1) PSELID 에 다시 WRITE 하여 선택을 커밋해야 엔진이 켜진다.
 *  2) WRITE 는 반드시 NON-SECURE alias(0x4xxx_xxxx)로 해야 한다. SECURE
 *     alias(0x5xxx_xxxx = DT_REG_ADDR) write 는 무효였다.
 *  3) 부팅 시(SYS_INIT)에 써두면 이후 WiFi/BLE init 단계에서 다시 지워진다.
 *     → 첫 I2C 사용 시점(런타임)에 1회 커밋해야 한다.
 * 따라서 모든 공개 함수 진입 시 i2c_bus_select_once() 를 호출한다. */
#define FLEXCOMM_PSELID_OFFSET 0xFF8u
#define FLEXCOMM_PERSEL_MASK   0x7u
#define FLEXCOMM_PERSEL_I2C    0x3u
/* secure(0x5xxx) → non-secure(0x4xxx) alias */
#define FLEXCOMM_NS_BASE       (DT_REG_ADDR(I2C_NODE) & ~0x10000000u)

static void i2c_bus_select_once(void)
{
	static bool s_selected;
	if (s_selected) {
		return;
	}
	s_selected = true;

	CLOCK_EnableClock(kCLOCK_Flexcomm2);   /* FC2 클럭 게이트 보장 (무해) */
	volatile uint32_t *pselid =
		(volatile uint32_t *)(FLEXCOMM_NS_BASE + FLEXCOMM_PSELID_OFFSET);
	uint32_t v = *pselid & ~FLEXCOMM_PERSEL_MASK;
	/* 핵심: PERSEL 을 같은 값(3)으로 다시 쓰면 재래치가 안 된다. 일단 다른
	 * 값(0=none)으로 바꿨다가 I2C(3)로 전환해야 엔진이 재연결된다. */
	*pselid = v | 0x0u;                    /* PERSEL = none (전환 유도) */
	*pselid = v | FLEXCOMM_PERSEL_I2C;     /* PERSEL = I2C */
}

int i2c_bus_init(void)
{
    if (!device_is_ready(i2c_dev)) {
        return -ENODEV;
    }
    return 0;
}

int i2c_bus_write(uint8_t addr_7b, const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0u) return -EINVAL;

    int rc;
    k_mutex_lock(&s_lock, K_FOREVER);
    i2c_bus_select_once();
    rc = i2c_write(i2c_dev, data, len, addr_7b);
    k_mutex_unlock(&s_lock);
    return rc;
}

int i2c_bus_read(uint8_t addr_7b, uint8_t *data, size_t len)
{
    if (data == NULL || len == 0u) return -EINVAL;

    int rc;
    k_mutex_lock(&s_lock, K_FOREVER);
    i2c_bus_select_once();
    rc = i2c_read(i2c_dev, data, len, addr_7b);
    k_mutex_unlock(&s_lock);
    return rc;
}

int i2c_bus_probe(uint8_t addr_7b)
{
    int rc;
    /* 표준 존재확인 = 0바이트 write (주소만 보내고 ACK 확인). */
    struct i2c_msg msg = {
        .buf   = NULL,
        .len   = 0,
        .flags = I2C_MSG_WRITE | I2C_MSG_STOP,
    };
    k_mutex_lock(&s_lock, K_FOREVER);
    i2c_bus_select_once();
    uint32_t t0 = k_uptime_get_32();
    rc = i2c_transfer(i2c_dev, &msg, 1, addr_7b);
    uint32_t dt = k_uptime_get_32() - t0;
    /* 진단: rc·소요시간으로 "not found" 원인 구분. dt 가 핵심 지표:
     *  rc=0,  dt~0ms       → ACK (센서 검출)
     *  rc=-5, dt~0ms       → 정상 NAK (그 주소에 장치 없음 / 배선·풀업·전원)
     *  dt~100ms [TIMEOUT]  → 엔진 미래치 (PSELID 수정이 이번 부팅에 안 먹음)
     * CFG 의 MSTEN 은 드라이버가 전송 시에만 켜므로 평소 0 = 정상(고장 지표 아님). */
    volatile uint32_t *fc2 = (volatile uint32_t *)0x40108000u;
    LOG_INF("probe 0x%02x: rc=%d dt=%ums CFG=0x%03x%s",
            addr_7b, rc, dt, fc2[0],
            (rc == 0) ? "" : (dt >= 90u ? " [TIMEOUT/no-latch]" : " [NAK]"));
    k_mutex_unlock(&s_lock);
    return (rc == 0) ? 0 : -ENODEV;
}

void i2c_bus_force_unlock(void)
{
    k_mutex_init(&s_lock);
}

int i2c_bus_write_read(uint8_t addr_7b,
                       const uint8_t *tx, size_t tx_len,
                       uint8_t       *rx, size_t rx_len)
{
    if (tx == NULL || rx == NULL)     return -EINVAL;
    if (tx_len == 0u || rx_len == 0u) return -EINVAL;

    int rc;
    k_mutex_lock(&s_lock, K_FOREVER);
    i2c_bus_select_once();
    rc = i2c_write_read(i2c_dev, addr_7b, tx, tx_len, rx, rx_len);
    k_mutex_unlock(&s_lock);
    return rc;
}
