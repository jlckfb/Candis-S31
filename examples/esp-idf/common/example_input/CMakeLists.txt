idf_component_register(
    SRCS "example_input.c"
    INCLUDE_DIRS "include"
    REQUIRES
        candis_s31
        rx8130ce
        driver
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
