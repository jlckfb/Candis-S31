idf_component_register(
    SRCS "camera_test_main.c"
    INCLUDE_DIRS "."
    REQUIRES candis_s31 esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
