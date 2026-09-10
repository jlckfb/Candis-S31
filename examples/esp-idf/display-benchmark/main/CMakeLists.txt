idf_component_register(
    SRCS "display_benchmark_main.c"
    REQUIRES
        example_board
        candis_s31
        lvgl
        esp_lvgl_port
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
