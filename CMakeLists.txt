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
