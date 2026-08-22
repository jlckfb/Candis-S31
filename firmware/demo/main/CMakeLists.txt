idf_component_register(
    SRCS
        "demo_main.c"
        "demo_board.c"
        "demo_apps.c"
        "services/svc_audio.c"
        "services/svc_input.c"
        "services/svc_net.c"
        "services/svc_power.c"
        "services/svc_storage.c"
        "ui/ui_manager.c"
        "ui/ui_menu.c"
        "ui/ui_watchface.c"
        "ui/ui_keyboard.c"
        "ui/fonts/candis_ui_20.c"
        "ui/fonts/candis_ui_24.c"
        "apps/app_recorder.c"
        "apps/app_player.c"
        "apps/app_test_center.c"
        "apps/app_display_test.c"
        "apps/app_wifi.c"
        "apps/app_ble.c"
        "apps/app_files.c"
        "apps/app_usb.c"
        "apps/app_led.c"
        "apps/app_settings.c"
        "apps/app_power.c"
        "apps/app_camera.c"
        "apps/app_sysinfo.c"
        "games/game_2048.c"
        "games/game_snake.c"
        "games/game_breakout.c"
    INCLUDE_DIRS "."
    REQUIRES
        candis_s31
        bt
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        esp_timer
        esp_lcd
        esp_lcd_touch_cst820
        rx8130ce
        fatfs
        usb_host_msc
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
