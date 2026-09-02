idf_component_register(
    SRCS
        "player_main.c"
        "board_init.c"
        "storage_service.c"
        "audio_service.c"
        "input_service.c"
        "player_utils.c"
        "player_video_backend.c"
        "video_player_explicit.c"
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
        esp_player
        esp_audio_render
        esp_video_codec
        esp_video_render
        gmf_audio
        gmf_core
        gmf_video
        media_lib_sal
        esp_extractor
        esp_audio_codec
        esp_psram
        spi_flash
        driver
        freertos
        lvgl
    EMBED_FILES
        "assets/NotoSansSC-Regular-sub.ttf"
        "assets/Candis_demo_460x460_30fps.avi"
        "assets/Candis_reference_460x460_30fps.mp4"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
