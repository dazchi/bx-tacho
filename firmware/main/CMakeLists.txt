# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Darren Chi
idf_component_register(
    SRCS
        "main.c"
        "sense_adc.c"
        "battery.c"
        "rpm.c"
        "load.c"
        "launch.c"
        "display.c"
        "font5x7.c"
        "settings.c"
        "cli.c"
        "eventlog.c"
        "icons.c"
        "vbat_cal.c"
        "bbp.c"
        "ble_console.c"
        "ble_ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_gptimer
        esp_driver_i2c
        esp_adc
        esp_lcd
        esp_timer
        esp_pm
        nvs_flash
        console
        esp_driver_usb_serial_jtag
        esp_app_format
        app_update
        efuse
        bt
        bbp_ext_api
)

# An optional Battle Pass extension (bbp_ext.h) is linked in when the build has
# one; bbp.c only calls it then
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(bbp_ext IN_LIST build_components)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE BXT_HAVE_BBP_EXT=1)
    endif()
    idf_component_optional_requires(PRIVATE bbp_ext)
endif()
