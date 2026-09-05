set(ESP32_CAMERA_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp32-camera")

set(CAMERA_TEST_SRCS "camera_test_main.c" "camera_visual.c")
set(CAMERA_TEST_INCLUDE_DIRS ".")
if(CONFIG_CAMERA_TEST_ESP32_CAMERA_OV5640)
    if(NOT EXISTS "${ESP32_CAMERA_DIR}/sensors/ov5640.c")
        message(FATAL_ERROR
            "esp32-camera 2.1.7 sources are missing at ${ESP32_CAMERA_DIR}")
    endif()
    list(APPEND CAMERA_TEST_SRCS
        "esp32_camera_ov5640_adapter.c"
        "${ESP32_CAMERA_DIR}/driver/sensor.c"
        "${ESP32_CAMERA_DIR}/sensors/ov5640.c")
    list(APPEND CAMERA_TEST_INCLUDE_DIRS
        "${ESP32_CAMERA_DIR}/driver/include"
        "${ESP32_CAMERA_DIR}/driver/private_include"
        "${ESP32_CAMERA_DIR}/sensors/private_include")
endif()

idf_component_register(
    SRCS ${CAMERA_TEST_SRCS}
    INCLUDE_DIRS ${CAMERA_TEST_INCLUDE_DIRS}
    REQUIRES candis_s31 esp_timer lvgl heap esp_video esp_driver_ppa
             esp_driver_uart driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
