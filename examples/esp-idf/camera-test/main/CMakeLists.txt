set(CAMERA_TEST_INCLUDE_DIRS ".")

idf_component_register(
    SRCS "camera_test_main.c" "camera_visual.c"
    INCLUDE_DIRS ${CAMERA_TEST_INCLUDE_DIRS}
    REQUIRES candis_s31 esp_timer lvgl heap esp_video
             esp_driver_uart driver esp_mm
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
