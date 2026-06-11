# Copyright 2026 zCube
# SPDX-License-Identifier: Apache-2.0
#
# Zephyr application build for frdm_rw612_blank.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(frdm_rw612_blank LANGUAGES C ASM)

# CMake on a Windows host injects MinGW import libs (kernel32, user32, ...)
# into CMAKE_*_STANDARD_LIBRARIES, and mbedtls/tf-psa-crypto pulls
# ws2_32 / bcrypt into its PUBLIC link interface for the host PSA RNG
# provider. None of these exist for the arm-zephyr-eabi cross toolchain.
# Strip just those host libs but keep the rest of the link interface intact
# so the cross-build still pulls in libtfpsacrypto.a / libmbedtls.a.
if(CMAKE_HOST_WIN32)
    set(CMAKE_C_STANDARD_LIBRARIES   "" CACHE STRING "" FORCE)
    set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
    set(_host_only_libs
        ws2_32 bcrypt kernel32 user32 gdi32 winspool
        shell32 ole32 oleaut32 uuid comdlg32 advapi32)
    foreach(_t IN ITEMS tfpsacrypto mbedtls mbedx509)
        if(TARGET ${_t})
            get_target_property(_libs ${_t} INTERFACE_LINK_LIBRARIES)
            if(_libs)
                list(REMOVE_ITEM _libs ${_host_only_libs})
                set_target_properties(${_t}
                    PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
            endif()
        endif()
    endforeach()
endif()

target_include_directories(app PRIVATE
  app
  app/matter
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

target_sources(app PRIVATE
  src/main.c

  app/app_main_zephyr.c
  app/i2c_bus.c

  app/matter/matter_hub_core.c
  app/matter/matter_prefs.c

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
  app/ir/ir_rx_nxp_gpio_ctimer.c
  app/ir/ir_tx_zephyr.c

  app/leds/led_strip_zephyr.c
  app/leds/led_task_zephyr.c

  app/nvm/app_nvram.c
  app/nvm/nvs_backend.c

  app/ota/app_ota.c
  app/ota/app_ota_http.c

  app/radar/app_radar_zephyr.c

  app/rotary_enc/rotary_enc.c

  app/sensors/amg8833.c
  app/sensors/amg8833_task.c
  app/sensors/bh1750.c
  app/sensors/bh1750_io_nxp_i2c.c
  app/sensors/lux_task.c
  app/sensors/mlx90640.c
  app/sensors/mlx90640_task.c

  app/tcp/app_tcp_client.c
  app/tcp/app_tcp_server.c

  app/wifi/app_wifi.c
)

# Intentionally excluded:
#   app/ir/ir_tx_nxp_ctimer.c
#   app/leds/sk6812.c
#   app/leds/sk6812_io_nxp_spi.c
#   app/multitask_init.c
#   app/ble/gatt_client/ble_central.c
#   app/ble/zephyr_bt_rand_shim.c

# ── OpenThread RCP host 링크 순서 보정 ──────────────────────────────────────
# Zephyr OT glue(zephyr/modules/openthread/CMakeLists.txt)는 ot_libs 에
# openthread-ftd 를 radio-spinel 보다 *먼저* 나열하고 --start-group 으로 묶지
# 않는다. radio_spinel.cpp 가 참조하는 otThreadErrorToString 은 ftd 아카이브의
# error_api.cpp 에만 있는데, ftd 가 링크 라인 앞쪽이라 그 시점엔 끌려오지 않아
# 뒤의 radio-spinel 에서 undefined reference 가 난다.
#
# 타겟 이름(openthread-ftd)으로 재추가하면 CMake 가 중복 제거(dedup)해 원래
# 위치에 머물러 효과가 없다. 따라서 두 아카이브의 *파일 경로*를 링크 끝에 한
# 번 더, --start-group/--end-group 으로 묶어 주입한다. 이 후행 그룹은 앞쪽에서
# 끌려오지 않은 객체까지 상호참조를 완전히 해소한다.
if(CONFIG_HDLC_RCP_IF AND CONFIG_OPENTHREAD_FTD)
  zephyr_link_libraries(
    -Wl,--start-group
    "$<TARGET_FILE:openthread-ftd>"
    "$<TARGET_FILE:openthread-radio-spinel>"
    -Wl,--end-group)
endif()
