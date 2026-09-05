# Test framework: domain suites are fixed-name files filled by parallel
# workstreams (spec E.0/E.2); GLOB picks up every tests/test_*.c so new
# suites need no CMake edit. The runner itself (svc_test.c) is explicit.
file(GLOB TEST_SRCS "tests/test_*.c")

idf_component_register(
    SRCS
        "demo_main.c"
        ${TEST_SRCS}
        "tests/svc_test.c"
        "tests/led_hw.c"
        "tests/display_diag.c"
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
        "ui/ui_perf.c"
        "ui/ui_page_walk.c"
        "ui/ui_widgets.c"
        "ui/fonts/candis_ui_20.c"
        "ui/fonts/candis_ui_24.c"
        "apps/app_recorder.c"
        "apps/app_player.c"
        "apps/app_test_center.c"
        "apps/app_test_run.c"
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
        esp_driver_bitscrambler
        esp_driver_cordic
        esp_driver_jpeg
        esp_driver_ppa
        esp_driver_tsens
        esp_mm
        esp_psram
        heap
        freertos
        lvgl
)
target_bitscrambler_add_src("tests/factory_bitscrambler.bsasm")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

if(CANDIS_DEMO_AUTOTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CANDIS_DEMO_AUTOTEST=1
    )
endif()
if(CANDIS_DEMO_CAMERA_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CANDIS_DEMO_CAMERA_BENCH=1
    )
endif()


if(CANDIS_DEMO_PAGE_WALK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        CONFIG_DEMO_PAGE_WALK=1
    )
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
