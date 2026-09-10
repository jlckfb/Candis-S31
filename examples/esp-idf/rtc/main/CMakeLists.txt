idf_component_register(
    SRCS "rtc_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        example_input
        candis_s31
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
