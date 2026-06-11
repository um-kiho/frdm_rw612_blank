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
#include "mlx90640_task.h"
#include "rotary_enc.h"
#include "ir_rx.h"
#include "ir_decode.h"
#include "ir_codec_samsung_ac.h"
#include "ir_aircon.h"
#include "audio_player.h"
#include "app_ota_http.h"

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
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_DBG);

#define APP_MAIN_PERIOD_MS 1000
#define LED0_NODE DT_ALIAS(led0)
#define RADAR_RX_DROP_FIRST_BYTES 1u
#define RADAR_RX_WARMUP_MS       120u
#define RADAR_HEX_DUMP_ENABLE    1   /* 0으로 바꾸면 HEX 덤프 출력 비활성 */
#define RADAR_HEX_DUMP_MAX_BYTES 64u  /* 콘솔 지연 방지: 콜백당 출력 바이트 제한 */

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
		LOG_INF("NVRAM loaded - connecting WiFi ssid=[%s]", s_nvram_cfg.ssid);
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
	printf("APP: TCP rx %u bytes\n", (unsigned)len);

	if (len == 0u || data == NULL) {
		printf("APP: TCP rx ignored (empty payload)\n");
		return;
	}

	printf("APP: TCP rx processing start\n");

	/* OTA trigger via TCP
	 * Format: "OTA:<image_url>"           - download only (manual commit)
	 *         "OTA:<image_url> --commit"   - download + auto commit + reboot
	 * Example:
	 *   OTA:http://172.28.176.1:8080/firmware/zephyr.bin --commit
	 */
	if (len > 4 && strncmp((const char *)data, "OTA:", 4) == 0) {
		printf("APP: TCP rx detected OTA command\n");
		/* Copy to a NUL-terminated buffer, strip trailing CR/LF */
		static char s_ota_cmd[APP_OTA_HTTP_URL_MAX + 16];
		size_t copy_len = (len < sizeof(s_ota_cmd) - 1u)
		                  ? len : sizeof(s_ota_cmd) - 1u;
		memcpy(s_ota_cmd, data, copy_len);
		s_ota_cmd[copy_len] = '\0';
		/* Strip trailing whitespace / CRLF */
		for (int i = (int)copy_len - 1; i >= 0 && (s_ota_cmd[i] == '\r'
		     || s_ota_cmd[i] == '\n' || s_ota_cmd[i] == ' '); --i) {
			s_ota_cmd[i] = '\0';
		}

		/* Split "OTA:<url>" or "OTA:<url> --commit" */
		char *url_start = s_ota_cmd + 4;  /* skip "OTA:" */
		bool  auto_commit = false;
		char *space = strchr(url_start, ' ');
		if (space != NULL) {
			*space = '\0';
			if (strcmp(space + 1, "--commit") == 0) {
				auto_commit = true;
			}
		}

		printf("APP: OTA auto_commit=%s\n", auto_commit ? "true" : "false");

		if (app_ota_http_is_running()) {
			LOG_WRN("OTA already running - ignoring TCP trigger");
			printf("APP: OTA already running -> ignore\n");
			return;
		}

		printf("APP: OTA start request -> app_ota_http_start()\n");

		app_ota_http_opts_t opts = {
			.image_url   = url_start,
			.header_url  = NULL,
			.auto_commit = auto_commit,
		};
		int rc = app_ota_http_start(&opts);
		LOG_INF("OTA TCP trigger: url=%s commit=%d rc=%d",
		        url_start, (int)auto_commit, rc);
		printf("APP: OTA start result rc=%d\n", rc);
		printf("APP: TCP rx processing done\n");
	} else {
		printf("APP: TCP rx not OTA command -> no action\n");
		printf("APP: TCP rx processing done\n");
	}
}

static void wifi_cb(app_wifi_evt_t evt, void *user_arg)
{
	ARG_UNUSED(user_arg);

	switch (evt) {
	case APP_WIFI_EVT_READY:
		if (app_nvram_is_valid(&s_nvram_cfg)) {
			LOG_INF("WiFi ready - connecting ssid=[%s]", s_nvram_cfg.ssid);
			printf("APP: wifi ready - connecting\n");
			app_wifi_connect(&s_nvram_cfg);
		} else {
			LOG_INF("WiFi ready - no credentials, waiting for provisioning");
			printf("APP: wifi ready, no credentials\n");
		}
		break;

	case APP_WIFI_EVT_CONNECTED:
		LOG_INF("WiFi connected - starting TCP client");
		printf("APP: wifi connected, starting TCP\n");
		if (app_tcp_client_start(&s_nvram_cfg, tcp_rx_cb, NULL) != 0) {
			LOG_ERR("TCP client start failed");
		}
		break;

	case APP_WIFI_EVT_DISCONNECTED:
		LOG_WRN("WiFi disconnected - stopping TCP client");
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
		/* Defer to system workq - calling net_mgmt from BT RX WQ overflows its stack */
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
	/* 출력 제한(64B): "RADAR[NNN]: " + "XX "*64 + " ..." + '\0' */
	static char s_hex_buf[256];

	ARG_UNUSED(user_arg);

	if (len < 5u) {
		return;
	}

#if RADAR_HEX_DUMP_ENABLE
	/* 콜백에서 과도한 printk를 피하기 위해 일부만 덤프 */
	size_t dump_len = (len > RADAR_HEX_DUMP_MAX_BYTES) ? RADAR_HEX_DUMP_MAX_BYTES : len;
	int pos = snprintk(s_hex_buf, sizeof(s_hex_buf), "RADAR[%u]:", (unsigned)len);
	for (size_t i = 0u; i < dump_len; ++i) {
		pos += snprintk(s_hex_buf + pos, (int)sizeof(s_hex_buf) - pos,
				" %02X", data[i]);
	}
	if (dump_len < len) {
		(void)snprintk(s_hex_buf + pos, (int)sizeof(s_hex_buf) - pos, " ...");
	}
	printk("%s\n", s_hex_buf);
#else
	ARG_UNUSED(s_hex_buf);
#endif
}

/* ── IR 수신 콜백 (시스템 워크큐에서 호출) ───────────────────────────────────
 * 캡처가 부하로 매 프레임 일부만 디코드되므로, Samsung AC 는 리모컨 반복분에
 * 걸쳐 "체크섬 통과한 7바이트 섹션"을 인덱스별로 수집한다. 3섹션(21B)이 다
 * 모이면 완성 프레임으로 1회 보고 → 한 번 눌러도 안정적으로 풀 코드 획득. */
static void ir_rx_frame_cb(const ir_symbol_t *syms, size_t count, void *user)
{
	ARG_UNUSED(user);

	static uint8_t s_sec[3][7];   /* 인덱스별 누적 섹션 */
	static bool    s_have[3];

	/* 섹션0 헤더가 없으면 Samsung AC 아님 → NEC/LG 시도 */
	uint8_t tmp[7];
	if (ir_decode_samsung_ac_section(syms, count, 0, tmp) < 0) {
		ir_decoded_t dec;
		if (ir_decode(syms, count, &dec) == 0 &&
		    dec.proto != IR_PROTO_SAMSUNG_AC) {
			printf("IR RX: %s addr=0x%04X cmd=0x%02X bits=%u%s\n",
			       ir_proto_name(dec.proto), dec.address, dec.command,
			       dec.bit_count, dec.repeat ? " (repeat)" : "");
		}
		return;
	}

	/* Samsung AC: 각 섹션을 오프셋 0/58/116 에서 독립 디코드 → 체크섬 통과분만
	 * 인덱스별 누적. 3섹션 다 모이면 보고.
	 * TODO(다음 세션): 버튼 전환 시 섹션0 손상 프레임에서 이전 버튼 sec0 + 새
	 * 버튼 sec2 가 섞일 수 있음. "섹션0 앵커"는 수신율을 떨어뜨려서 보류. 대신
	 * 타임아웃 리셋(예: 마지막 수집 후 300ms 무수집 시 누적 클리어) 등으로 보완. */
	for (uint8_t s = 0; s < 3u; ++s) {
		if (ir_decode_samsung_ac_section(syms, count, s, tmp) != 1) {
			continue;
		}
		if (!ir_codec_samsung_ac_verify(tmp)) {
			continue;
		}
		if (s == 0u && s_have[0] && memcmp(tmp, s_sec[0], 7u) != 0) {
			s_have[1] = false;   /* 다른 버튼 → 뒤 섹션 누적 리셋 */
			s_have[2] = false;
		}
		memcpy(s_sec[s], tmp, 7u);
		s_have[s] = true;
	}

	if (s_have[0] && s_have[1] && s_have[2]) {
		printf("IR RX: Samsung AC (21B):");
		for (uint8_t s = 0; s < 3u; ++s) {
			for (uint8_t i = 0; i < 7u; ++i) {
				printf(" %02X", s_sec[s][i]);
			}
		}
		printf("\n");
		s_have[0] = s_have[1] = s_have[2] = false;
	}
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
	printf("APP: BLE disconnected reason=0x%02X - restarting adv\n", reason);

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

	/* Start advertising first - device name remains visible even if service init fails */
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
		LOG_WRN("No valid NVRAM - WiFi will not auto-connect");
		printf("APP: no nvram credentials, waiting for provisioning\n");
	}

	/* app_wifi_connect() is deferred to wifi_cb(APP_WIFI_EVT_READY),
	 * which fires only after NET_EVENT_IF_UP - i.e. NXP firmware loaded. */
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
	printk("START\n");

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
	 * BH1750 (0x23), AMG8833 (0x69, ADDR=HIGH) - FC2 I2C (GPIO16/17)
	 * 미연결 상태에서 활성화 시 I2C 버스 hang 발생 */
#if defined(CONFIG_APP_SENSORS_ENABLE)
	if (lux_task_start(500, NULL, NULL) != 0) {
		LOG_WRN("lux_task_start failed");
	} else {
		LOG_INF("BH1750 lux task created (I2C probe pending)");
	}
	/* 열화상 센서: Kconfig choice 로 택1 (AMG8833 / MLX90640 / none) */
#if defined(CONFIG_APP_THERMAL_AMG8833)
	if (amg_task_start(0x69, 500, NULL, NULL) != 0) {
		LOG_WRN("amg_task_start failed");
	} else {
		LOG_INF("AMG8833 task created (I2C probe pending)");
	}
#elif defined(CONFIG_APP_THERMAL_MLX90640)
	/* MLX90640 (0x33) 32x24 thermal - FC2 I2C (GPIO16/17), 4 Hz/sub-page */
	if (mlx_task_start(MLX90640_ADDR_DEFAULT, MLX90640_RR_4HZ, 500, NULL, NULL) != 0) {
		LOG_WRN("mlx_task_start failed");
	} else {
		LOG_INF("MLX90640 task created (I2C probe pending)");
	}
#else
	LOG_INF("No thermal sensor selected (BH1750 only)");
#endif
#else
	LOG_INF("Sensors disabled (CONFIG_APP_SENSORS_ENABLE not set)");
	printf("APP: sensors disabled - enable CONFIG_APP_SENSORS_ENABLE when connected\n");
#endif

	/* EC16 rotary encoder (GPIO4=A, GPIO5=B) - pin config + ANY_EDGE interrupt.
	 * gpio_pin_configure_dt(GPIO_INPUT) 가 DT 의 GPIO_PULL_UP 까지 적용한다. */
	if (rotary_enc_init() != 0) {
		LOG_WRN("rotary_enc_init failed");
		printf("APP: rotary init failed\n");
	} else {
		printf("APP: rotary encoder ready (GPIO4/5)\n");
	}

	/* IR RX (TSDP341, GPIO18 active-low) - frame capture -> decode callback */
	ir_rx_set_callback(ir_rx_frame_cb, NULL);
	if (ir_rx_init() == 0 && ir_rx_start() == 0) {
		printf("APP: IR RX started (GPIO18 TSDP341)\n");
	} else {
		printf("APP: IR RX init/start failed\n");
	}

	/* Radar UART(FC0): BLE 준비와 무관하게 먼저 시작해서 RX 콘솔 확인 가능 */
	{
		static const char s_radar_start_msg[] = "START\r\n";
		int rrc;
		printf("APP: svc-init: radar\n");
		rrc = app_radar_start(radar_rx_cb, NULL);
		if (rrc != 0) {
			printf("APP: radar start failed rc=%d\n", rrc);
		} else {
			printf("APP: radar started\n");
			rrc = app_radar_send(s_radar_start_msg, sizeof(s_radar_start_msg) - 1u);
			if (rrc != 0) {
				printf("APP: radar start msg send failed rc=%d\n", rrc);
			} else {
				printf("APP: radar start msg sent on FC0\n");
			}
		}
	}

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

		/* 로터리 카운트/버튼 변화 출력 (ISR 누적값을 1초마다 폴링) */
		{
			static int32_t s_last_rot = 0;
			static int32_t s_last_sw  = 0;
			int32_t rot = rotary_enc_get_count();
			int32_t sw  = rotary_enc_get_sw_count();
			if (rot != s_last_rot) {
				printf("ROT: count=%d (delta=%d)\n", rot, rot - s_last_rot);
				s_last_rot = rot;
			}
			if (sw != s_last_sw) {
				s_last_sw = sw;
				/* SW 4-step sequence: 1=ON, 2=TEMP_UP, 3=TEMP_DOWN, 4=OFF */
				int seq = sw % 4;
				ir_aircon_action_t action;
				const char *action_name;
				switch (seq) {
				case 1:
					action = IR_AIRCON_ACTION_POWER_ON;
					action_name = "POWER_ON";
					break;
				case 2:
					action = IR_AIRCON_ACTION_TEMP_UP;
					action_name = "TEMP_UP";
					break;
				case 3:
					action = IR_AIRCON_ACTION_TEMP_DOWN;
					action_name = "TEMP_DOWN";
					break;
				default:
					action = IR_AIRCON_ACTION_POWER_OFF;
					action_name = "POWER_OFF";
					break;
				}

				int trc = ir_aircon_send(IR_AIRCON_BRAND_SAMSUNG, action);
				printf("ROT: SW press #%d -> AC %s (tx rc=%d)\n",
				       sw, action_name, trc);
			}
		}

		if (s_ble_ready) {
			uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000LL);

			/* Latch APPLIED state once TCP is connected */
			if (!s_prov_applied && app_tcp_client_is_connected()) {
				s_prov_applied = true;
				app_prov_set_state(APP_PROV_STATE_APPLIED);
				LOG_INF("TCP connected - provisioning applied");
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
		//	app_main_dump_stacks();
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
