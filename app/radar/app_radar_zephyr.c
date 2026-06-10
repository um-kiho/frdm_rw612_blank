/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Radar UART backend — Zephyr DMA async 구현
 *
 * Device tree alias: radar-uart = &flexcomm0  (GPIO2=TX, GPIO3=RX, 115200 8N1)
 * DMA: DMA0 채널 0(RX), 1(TX)
 *
 * RX 흐름:
 *   uart_rx_enable() → DMA가 s_rx_buf[] 채움 →
 *   UART_RX_RDY 이벤트에서 app_radar_rx_cb_t 즉시 호출
 *
 * 더블 버퍼링: UART_RX_BUF_REQUEST 시 다음 버퍼 즉시 제공 → 수신 끊김 없음
 *
 * TX: uart_tx() DMA 전송
 */

#include "radar/app_radar.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(app_radar, LOG_LEVEL_INF);

#define RADAR_UART_NODE  DT_ALIAS(radar_uart)

/* RX 더블 버퍼 */
#define RX_BUF_SIZE   512u
#define RX_BUF_COUNT  2u

/* RX 비활성 타임아웃: 마지막 바이트 수신 후 2 ms 무신호 시 UART_RX_RDY 강제 발생 -->20us */
#define RX_TIMEOUT_US 20u

static const struct device *s_uart;
static app_radar_rx_cb_t    s_rx_cb;
static void                *s_cb_arg;
static volatile bool        s_running;
static bool                 s_rx_seen;

/* 더블 버퍼 */
static uint8_t s_rx_buf[RX_BUF_COUNT][RX_BUF_SIZE];
static uint8_t s_buf_idx;   /* 현재 uart_rx_enable에 등록된 버퍼 인덱스 */

/* UART async 콜백 */
static void uart_async_cb(const struct device *dev,
			  struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {

	case UART_RX_RDY:
		if (!s_rx_seen) {
			s_rx_seen = true;
			printk("RADAR: first RX_RDY len=%u\n", (unsigned)evt->data.rx.len);
		}
		if (s_rx_cb && evt->data.rx.len > 0u) {
			s_rx_cb(evt->data.rx.buf + evt->data.rx.offset,
				evt->data.rx.len,
				s_cb_arg);
		}
		break;

	case UART_RX_BUF_REQUEST:
		/* 다음 버퍼 즉시 제공 → 수신 끊김 방지 */
		s_buf_idx = (s_buf_idx + 1u) % RX_BUF_COUNT;
		uart_rx_buf_rsp(dev, s_rx_buf[s_buf_idx], RX_BUF_SIZE);
		break;

	case UART_RX_BUF_RELEASED:
		/* 정적 버퍼 — 별도 해제 불필요 */
		break;

	case UART_RX_DISABLED:
		/* 타임아웃 또는 오류 후 자동 재시작 */
		if (s_running) {
			s_buf_idx = (s_buf_idx + 1u) % RX_BUF_COUNT;
			uart_rx_enable(dev,
				       s_rx_buf[s_buf_idx],
				       RX_BUF_SIZE,
				       RX_TIMEOUT_US);
		}
		break;

	default:
		break;
	}
}

int app_radar_start(app_radar_rx_cb_t rx_cb, void *user_arg)
{
	s_uart = DEVICE_DT_GET(RADAR_UART_NODE);
	if (!device_is_ready(s_uart)) {
		LOG_ERR("radar-uart device not ready");
		printk("RADAR: device not ready\n");
		return -ENODEV;
	}

	s_rx_cb  = rx_cb;
	s_cb_arg = user_arg;

	int rc = uart_callback_set(s_uart, uart_async_cb, NULL);
	if (rc != 0) {
		LOG_ERR("uart_callback_set failed: %d", rc);
		printk("RADAR: uart_callback_set failed rc=%d\n", rc);
		return rc;
	}

	s_buf_idx = 0u;
	rc = uart_rx_enable(s_uart, s_rx_buf[s_buf_idx], RX_BUF_SIZE, RX_TIMEOUT_US);
	if (rc != 0) {
		LOG_ERR("uart_rx_enable failed: %d", rc);
		printk("RADAR: uart_rx_enable failed rc=%d\n", rc);
		return rc;
	}

	s_rx_seen = false;
	s_running = true;
	LOG_INF("radar UART started (DMA async, immediate cb, buf=%ux%u B)",
		RX_BUF_COUNT, RX_BUF_SIZE);
	printk("RADAR: started immediate-cb buf=%ux%u\n",
	       RX_BUF_COUNT, RX_BUF_SIZE);
	return 0;
}

void app_radar_stop(void)
{
	if (s_running && s_uart) {
		s_running = false;
		uart_rx_disable(s_uart);
	}
}

bool app_radar_is_running(void)
{
	return s_running;
}

int app_radar_send(const void *data, size_t len)
{
	if (!s_running || !s_uart) {
		return -ENODEV;
	}
	if (data == NULL || len == 0u) {
		return -EINVAL;
	}

	/* uart_tx: DMA TX, 100 ms 타임아웃 */
	return uart_tx(s_uart, (const uint8_t *)data, len, SYS_FOREVER_US);
}
