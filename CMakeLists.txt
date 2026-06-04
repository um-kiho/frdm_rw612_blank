# Copyright 2026 zCube
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr application build for frdm_rw612_blank.
#
# NOTE: This file was reconstructed (2026-06-02) after the original was
# accidentally overwritten by an unrelated JSON-config build script. The
# source list below mirrors the last known-good build (debug/, 2026-05-29).
# Mutually-exclusive alternates and not-yet-wired WIP modules are intentionally
# excluded (see the commented block at the bottom).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(frdm_rw612_blank LANGUAGES C ASM)

# ── Application include directories ──────────────────────────────────────────
target_include_directories(app PRIVATE
	app
	app/.matter
	app/audio
	app/ble
	app/ble/gatt_servers
	app/ir
	app/leds
	app/nvm
	app/ota
	app/radar
	app/rotary_enc
	app/sensors
	app/tcp
	app/wifi
)

# ── Application sources ──────────────────────────────────────────────────────
target_sources(app PRIVATE
	src/main.c

	app/app_main_zephyr.c
	app/i2c_bus.c

	app/.matter/matter_hub_core.c
	app/.matter/matter_prefs.c

	app/audio/audio_player.c
	app/audio/es8388.c

	app/ble/gatt_servers/svc_ctrl.c
	app/ble/gatt_servers/svc_ir.c
	app/ble/gatt_servers/svc_led.c
	app/ble/gatt_servers/svc_matter.c
	app/ble/gatt_servers/svc_prov.c
	app/ble/gatt_servers/svc_status.c

	app/ir/ir_aircon.c
	app/ir/ir_codec_samsung_ac.c
	app/ir/ir_decode.c
	app/ir/ir_tx_zephyr.c
	app/ir/ir_rx_nxp_gpio_ctimer.c

	app/leds/led_strip_zephyr.c
	app/leds/led_task_zephyr.c

	app/nvm/app_nvram.c
	app/nvm/nvs_backend.c

	app/radar/app_radar_zephyr.c

	app/rotary_enc/rotary_enc.c

	app/sensors/amg8833.c
	app/sensors/amg8833_task.c
	app/sensors/bh1750.c
	app/sensors/bh1750_io_nxp_i2c.c
	app/sensors/lux_task.c

	app/tcp/app_tcp_client.c
	app/tcp/app_tcp_server.c

	app/wifi/app_wifi.c
)

# ── Intentionally excluded ───────────────────────────────────────────────────
# Alternates (would clash with the files above):
#   app/ir/ir_tx_nxp_ctimer.c        (alternate to ir_tx_zephyr.c)
#   app/leds/sk6812.c
#   app/leds/sk6812_io_nxp_spi.c     (alternates to led_strip_zephyr.c)
# Example / not auto-built:
#   app/multitask_init.c             (demo threads, auto-start)
# Not yet wired into app_main_zephyr.c (enable when integrated):
#   app/ota/app_ota.c
#   app/ota/app_ota_http.c
#   app/ble/gatt_servers/svc_ota.c
#   app/ble/gatt_client/ble_central.c
#   app/ble/zephyr_bt_rand_shim.c
