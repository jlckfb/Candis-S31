idf_component_register(
    SRCS
        "factory_main.c"
        "factory_console.c"
        "factory_peripherals.c"
        "factory_report.c"
    INCLUDE_DIRS "."
    REQUIRES
        candis_s31
        bt
        console
        esp_app_format
        esp_event
        esp_netif
        esp_psram
        esp_timer
        esp_wifi
        fusb303b
        heap
        nvs_flash
        spi_flash
        usb
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
