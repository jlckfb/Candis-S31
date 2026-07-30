idf_component_register(
    SRCS
        "factory_main.c"
        "factory_console.c"
        "factory_peripherals.c"
        "factory_report.c"
    INCLUDE_DIRS "."
    REQUIRES
        candis_s31
        console
        esp_app_format
        esp_psram
        esp_timer
        heap
        spi_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
