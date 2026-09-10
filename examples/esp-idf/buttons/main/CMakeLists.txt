idf_component_register(
    SRCS "buttons_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        example_input
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
