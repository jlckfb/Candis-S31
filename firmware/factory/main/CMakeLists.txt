idf_component_register(
    SRCS
        "factory_main.c"
        "factory_audio.c"
        "factory_camera.c"
        "factory_console.c"
        "factory_diag.c"
        "factory_display.c"
        "factory_input.c"
        "factory_peripherals.c"
        "factory_power.c"
        "factory_rf.c"
        "factory_accel.c"
        "factory_report.c"
        "factory_rtc.c"
        "factory_storage.c"
        "factory_touch.c"
        "factory_usb.c"
    INCLUDE_DIRS "."
    REQUIRES
        candis_s31
        bt
        console
        esp_app_format
        esp_event
        esp_netif
        esp_phy
        esp_driver_jpeg
        esp_driver_cordic
        esp_driver_ppa
        esp_driver_bitscrambler
        esp_psram
        esp_timer
        esp_wifi
        fusb303b
        heap
        nvs_flash
        spi_flash
        usb
        usb_host_msc
)

target_bitscrambler_add_src("factory_bitscrambler.bsasm")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
