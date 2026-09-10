idf_component_register(
    SRCS "led_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        candis_s31
        led_indicator
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
