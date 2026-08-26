idf_component_register(
    SRCS
        "player_main.c"
        "board_init.c"
        "storage_service.c"
        "audio_service.c"
        "input_service.c"
        "aviparser.c"
        "video_player.c"
        "ui_player.c"
    INCLUDE_DIRS "."
    REQUIRES
        candis_s31
        nvs_flash
        esp_event
        esp_timer
        esp_lcd
        esp_lcd_touch_cst820
        fatfs
        esp_codec_dev
        spi_flash
        driver
        freertos
        lvgl
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
