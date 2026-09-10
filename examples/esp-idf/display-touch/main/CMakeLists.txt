idf_component_register(
    SRCS "display_touch_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        candis_s31
        lvgl
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
