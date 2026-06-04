/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr-compatible application entry.
 *
 * The existing app/app_main.c tree depends on additional NXP SDK and
 * FreeRTOS components that are not wired into this sample project yet.
 * This file provides the same app_main_task() entry point so main() can
 * call into the app layer while the larger integration is brought in
 * incrementally.
 */

#include "app_main.h"

#include "ble/gatt_servers/svc_ctrl.h"
#include "ble/gatt_servers/svc_ir.h"
#include "ble/gatt_servers/svc_led.h"
#include "ble/gatt_servers/svc_matter.h"
#include "ble/gatt_servers/svc_prov.h"
#include "ble/gatt_servers/svc_status.h"

#include "app_nvram.h"
#include "radar/app_radar.h"
#include "matter_hub_core.h"
#include "wifi/app_wifi.h"
#include "tcp/app_tcp_client.h"
#include "lux_task.h"
#include "amg8833_task.h"
#if defined(CONFIG_APP_AUDIO_ES8388)
#include "audio_player.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#if defined(CONFIG_BT_CLASSIC)
#include <zephyr/bluetooth/classic/classic.h>
#endif
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_DBG);

#define APP_MAIN_PERIOD_MS 1000
#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static const struct bt_data s_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static volatile bool s_ble_ready;
static uint32_t s_led_manual_hold_s;
static app_nvram_data_t s_nvram_cfg;
static volatile bool s_prov_applied;

/* Deferred work: run WiFi connect from system workq, not BT RX WQ */
static struct k_work s_prov_connect_work;
static struct k_work s_prov_reset_work;

static void prov_connect_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (app_nvram_load(&s_nvram_cfg) == APP_NVRAM_OK) {
		LOG_INF("NVRAM loaded — connecting WiFi ssid=[%s]", s_nvram_cfg.ssid);
		printf("APP: connecting wifi ssid=[%s]\n", s_nvram_cfg.ssid);
		app_wifi_connect(&s_nvram_cfg);
	} else {
		LOG_ERR("NVRAM load failed after commit");
	}
}

static void prov_reset_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	app_tcp_client_stop();
	app_wifi_disconnect();
	s_prov_applied = false;
}

static void tcp_rx_cb(const uint8_t *data, size_t len, void *user_arg)
{
	ARG_UNUSED(user_arg);
	LOG_INF("TCP rx %u bytes", (unsigned)len);
}

static void wifi_cb(app_wifi_evt_t evt, void *user_arg)
{
	ARG_UNUSED(user_arg);

	switch (evt) {
	case APP_WIFI_EVT_READY:
		if (app_nvram_is_valid(&s_nvram_cfg)) {
			LOG_INF("WiFi ready — connecting ssid=[%s]", s_nvram_cfg.ssid);
			printf("APP: wifi ready — connecting\n");
			app_wifi_connect(&s_nvram_cfg);
		} else {
			LOG_INF("WiFi ready — no credentials, waiting for provisioning");
			printf("APP: wifi ready, no credentials\n");
		}
		break;

	case APP_WIFI_EVT_CONNECTED:
		LOG_INF("WiFi connected — starting TCP client");
		printf("APP: wifi connected, starting TCP\n");
		if (app_tcp_client_start(&s_nvram_cfg, tcp_rx_cb, NULL) != 0) {
			LOG_ERR("TCP client start failed");
		}
		break;

	case APP_WIFI_EVT_DISCONNECTED:
		LOG_WRN("WiFi disconnected — stopping TCP client");
		printf("APP: wifi disconnected\n");
		app_tcp_client_stop();
		s_prov_applied = false;
		break;

	case APP_WIFI_EVT_AUTH_FAILED:
		LOG_ERR("WiFi auth failed");
		printf("APP: wifi auth failed\n");
		break;

	default:
		break;
	}
}

static void prov_cb(app_prov_evt_t evt, void *user_arg)
{
	ARG_UNUSED(user_arg);

	printf("APP: prov evt=%d\n", (int)evt);

	switch (evt) {
	case APP_PROV_EVT_COMMIT_OK:
		/* Defer to system workq — calling net_mgmt from BT RX WQ overflows its stack */
		k_work_submit(&s_prov_connect_work);
		break;

	case APP_PROV_EVT_RESET:
		LOG_INF("Provisioning reset");
		k_work_submit(&s_prov_reset_work);
		break;

	default:
		break;
	}
}

static void ctrl_cb(app_ctrl_evt_t evt, void *user_arg)
{
	ARG_UNUSED(user_arg);

	printf("APP: ctrl evt=%d\n", (int)evt);
	app_ctrl_respond(APP_CTRL_RSP_OK);
}

static void publish_status(uint32_t uptime_s)
{
	bool wifi_ok = app_wifi_is_connected();
	bool tcp_ok  = app_tcp_client_is_connected();

	app_status_report_t report = {
		.wifi_state = wifi_ok ? 1U : 0U,
		.tcp_state  = tcp_ok  ? 1U : 0U,
		.prov_state = s_prov_applied ? APP_PROV_STATE_APPLIED : APP_PROV_STATE_IDLE,
		.sensor_state = s_ble_ready ? 1U : 0U,
		.uptime_s = uptime_s,
		.sensor_rx_count = 0U,
		.lux1_err = 0U,
		.lux2_err = 0U,
		.lux1_x100 = 0U,
		.lux2_x100 = 0U,
		.amg_err = 0,
		._amg_pad = 0U,
		.amg_min_q2 = 0,
		.amg_avg_q2 = 0,
		.amg_max_q2 = 0,
		.radar_rx_bytes = 0U,
	};

	app_status_publish(&report);
}

void app_main_led_manual_override(uint32_t hold_s)
{
	s_led_manual_hold_s = hold_s;
}

static void radar_rx_cb(const uint8_t *data, size_t len, void *user_arg)
{
	ARG_UNUSED(user_arg);
	LOG_DBG("radar UART rx %u bytes", (unsigned)len);
	/* TODO: process radar UART data here */
	(void)data;
}

#if defined(CONFIG_BT_CLASSIC)
static enum bt_br_conn_req_rsp br_conn_req_cb(const bt_addr_t *addr, uint32_t cod)
{
	ARG_UNUSED(addr);
	ARG_UNUSED(cod);
	return BT_BR_CONN_REQ_ACCEPT_PERIPHERAL;
}
#endif

/* ── BLE 연결 콜백 ─────────────────────────────────────────────────────── */
static void ble_connected_cb(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	if (err != 0) {
		LOG_WRN("BLE connect failed addr=[%s] err=0x%02X", addr, err);
		return;
	}
	LOG_INF("BLE connected addr=[%s]", addr);
	printf("APP: BLE connected addr=[%s]\n", addr);
}

static void ble_disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BLE disconnected addr=[%s] reason=0x%02X", addr, reason);
	printf("APP: BLE disconnected reason=0x%02X — restarting adv\n", reason);

	/* disconnect 후 광고 재시작 → 재접속 허용 */
	int rc = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
				 s_ad, ARRAY_SIZE(s_ad), NULL, 0);
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("adv restart failed err=%d", rc);
	}
}

BT_CONN_CB_DEFINE(s_conn_cb) = {
	.connected    = ble_connected_cb,
	.disconnected = ble_disconnected_cb,
};

static void bt_ready_cb(int err)
{
	if (err != 0) {
		LOG_ERR("bt_enable failed err=%d", err);
		printf("APP: bt_enable err=%d\n", err);
		return;
	}

	k_work_init(&s_prov_connect_work, prov_connect_work_fn);
	k_work_init(&s_prov_reset_work,   prov_reset_work_fn);

	LOG_INF("BLE stack ready");
	printf("APP: BLE stack ready\n");

	/* 광고를 먼저 시작 — 서비스 init 실패와 무관하게 장치 이름이 보임 */
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
			      s_ad, ARRAY_SIZE(s_ad), NULL, 0);
	if (err != 0) {
		LOG_ERR("adv start failed err=%d", err);
		printf("APP: adv start err=%d\n", err);
		return;
	}
	LOG_INF("Advertising started name=[%s]", CONFIG_BT_DEVICE_NAME);
	printf("APP: advertising started name=[%s]\n", CONFIG_BT_DEVICE_NAME);
	printf("APP: svc-init: nvram\n");
	(void)app_nvram_init();
	printf("APP: svc-init: matter_hub\n");
	(void)matter_hub_init();
	printf("APP: svc-init: prov\n");
	(void)app_prov_init(prov_cb, NULL);
	printf("APP: svc-init: status\n");
	(void)app_status_init();
	printf("APP: svc-init: ctrl\n");
	(void)app_ctrl_init(ctrl_cb, NULL);
	printf("APP: svc-init: ir\n");
	(void)app_ir_svc_init();
	printf("APP: svc-init: radar\n");
	(void)app_radar_start(radar_rx_cb, NULL);

	printf("APP: svc-init: led_svc\n");
	(void)app_led_svc_init();
	printf("APP: svc-init: matter_svc\n");
	(void)app_matter_svc_init();
#if defined(CONFIG_APP_AUDIO_ES8388)
	printf("APP: svc-init: es8388\n");
	static const audio_player_config_t s_audio_cfg = {
		.sample_rate = 44100u,
		.es8388_vol  = 0x10u,   /* ~-16 dB */
		.state_cb    = NULL,
		.user_arg    = NULL,
	};
	if (audio_player_init(&s_audio_cfg) != 0) {
		LOG_WRN("ES8388 init submit failed");
	} else {
		LOG_INF("ES8388 init pending (I2C probe in workqueue)");
	}
#endif

	printf("APP: all services initialized\n");
	LOG_INF("All services initialized");

	/* Load credentials into s_nvram_cfg before WiFi init so the READY
	 * callback can call app_wifi_connect() immediately when the NXP
	 * firmware finishes loading (NET_EVENT_IF_UP). */
	memset(&s_nvram_cfg, 0, sizeof(s_nvram_cfg));
	if (app_nvram_load(&s_nvram_cfg) == APP_NVRAM_OK &&
	    app_nvram_is_valid(&s_nvram_cfg)) {
		LOG_INF("NVRAM: ssid=[%s] pw=[%s] sec=[%s] host=[%s] port=%u",
			s_nvram_cfg.ssid, s_nvram_cfg.password,
			s_nvram_cfg.security, s_nvram_cfg.host_ip,
			s_nvram_cfg.port);
		printf("APP: nvram ssid=[%s] pw=[%s] sec=[%s] host=[%s] port=%u\n",
			s_nvram_cfg.ssid, s_nvram_cfg.password,
			s_nvram_cfg.security, s_nvram_cfg.host_ip,
			s_nvram_cfg.port);
	} else {
		LOG_WRN("No valid NVRAM — WiFi will not auto-connect");
		printf("APP: no nvram credentials, waiting for provisioning\n");
	}

	/* app_wifi_connect() is deferred to wifi_cb(APP_WIFI_EVT_READY),
	 * which fires only after NET_EVENT_IF_UP — i.e. NXP firmware loaded. */
	printf("APP: svc-init: wifi\n");
	if (app_wifi_init(wifi_cb, NULL) != 0) {
		LOG_ERR("WiFi init failed");
		printf("APP: wifi init failed\n");
	}

	s_ble_ready = true;
}

void app_main_task(void *arg)
{
	bool led_state = true;
	uint32_t blink_count = 0U;
	int ret;

	ARG_UNUSED(arg);

	LOG_INF("=== zCubeTarget boot ===");
	printf("APP: app_main_task started\n");

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("led0 not ready");
		printf("APP: led0 is not ready\n");
		return;
	}
	LOG_INF("GPIO led0 ready");

	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0) {
		LOG_ERR("led0 configure failed");
		printf("APP: failed to configure led0\n");
		return;
	}
	LOG_INF("GPIO led0 configured");
#if 0
	/* === GPIO PIN TEST: GPIO16(500ms) / GPIO17(1s) 토글 무한반복 ===
	 * I2C 핀(FC2 SDA/SCL) 출력 동작 확인용. 테스트 완료 후 이 블록 제거. */
	{
		const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(hsgpio0));
		if (!device_is_ready(gpio0)) {
			LOG_ERR("gpio0 not ready");
			return;
		}
		gpio_pin_configure(gpio0, 16, GPIO_OUTPUT_LOW);
		gpio_pin_configure(gpio0, 17, GPIO_OUTPUT_LOW);
		LOG_INF("GPIO pin test start: GPIO16=500ms GPIO17=1s");

		int cnt = 0;
		while (1) {
			gpio_pin_toggle(gpio0, 16);         /* 500 ms 주기 */
			if (cnt % 2 == 0) {
				gpio_pin_toggle(gpio0, 17);     /* 1000 ms 주기 */
			}
			cnt++;
			k_msleep(500);
		}
	}
	/* === GPIO PIN TEST END === */
#endif
	/* I2C 센서: 하드웨어 연결 후 활성화
	 * BH1750 (0x23), AMG8833 (0x69, ADDR=HIGH) — FC2 I2C (GPIO16/17)
	 * 미연결 상태에서 활성화 시 I2C 버스 hang 발생 */
#if defined(CONFIG_APP_SENSORS_ENABLE)
	if (lux_task_start(500, NULL, NULL) != 0) {
		LOG_WRN("lux_task_start failed");
	} else {
		LOG_INF("BH1750 lux task created (I2C probe pending)");
	}
	if (amg_task_start(0x69, 500, NULL, NULL) != 0) {
		LOG_WRN("amg_task_start failed");
	} else {
		LOG_INF("AMG8833 task created (I2C probe pending)");
	}
#else
	LOG_INF("Sensors disabled (CONFIG_APP_SENSORS_ENABLE not set)");
	printf("APP: sensors disabled — enable CONFIG_APP_SENSORS_ENABLE when connected\n");
#endif

	LOG_INF("Starting BLE stack (bt_enable)...");
	printf("APP: calling bt_enable...\n");
	ret = bt_enable(bt_ready_cb);
	if (ret != 0) {
		LOG_ERR("bt_enable returned %d", ret);
		printf("APP: bt_enable start failed ret=%d\n", ret);
	} else {
		LOG_INF("bt_enable accepted, waiting for callback...");
		printf("APP: bt_enable accepted, waiting for callback\n");
	}

	while (1) {
		if (gpio_pin_toggle_dt(&led) < 0) {
			printf("APP: failed to toggle led0\n");
			return;
		}

		led_state = !led_state;
		blink_count++;

		if (s_ble_ready) {
			uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000LL);

			/* Latch APPLIED state once TCP is connected */
			if (!s_prov_applied && app_tcp_client_is_connected()) {
				s_prov_applied = true;
				app_prov_set_state(APP_PROV_STATE_APPLIED);
				LOG_INF("TCP connected — provisioning applied");
				printf("APP: provisioning applied (TCP up)\n");
			}

			matter_hub_on_tick(uptime_s);
			publish_status(uptime_s);
			app_matter_svc_publish_status();
			if (s_led_manual_hold_s > 0u) {
				s_led_manual_hold_s--;
			}
		}

		/* 30초마다 전체 스레드 스택 사용량 출력 */
		if ((blink_count % 30u) == 0u) {
			app_main_dump_stacks();
		}

		k_msleep(APP_MAIN_PERIOD_MS);
	}
}

/* ── 스레드 스택 덤프 ───────────────────────────────────────────────────── */
static void stack_dump_cb(const struct k_thread *t, void *user_data)
{
	ARG_UNUSED(user_data);

	size_t unused = 0;
	(void)k_thread_stack_space_get(t, &unused);

	size_t total = t->stack_info.size;
	size_t used  = total - unused;

	const char *name = k_thread_name_get((struct k_thread *)t);

	printf("  %-20s total=%5u  used=%5u  free=%5u  %3u%%\n",
	       (name != NULL) ? name : "?",
	       (unsigned)total,
	       (unsigned)used,
	       (unsigned)unused,
	       (unsigned)(used * 100u / total));
}

void app_main_dump_stacks(void)
{
	printf("\n[STACK] uptime=%lld s\n", k_uptime_get() / 1000LL);
	printf("  %-20s %6s  %6s  %6s  %s\n",
	       "thread", "total", "used", "free", "usage");
	printf("  %s\n",
	       "------------------------------------------------------------");
	k_thread_foreach(stack_dump_cb, NULL);
	printf("\n");
}
