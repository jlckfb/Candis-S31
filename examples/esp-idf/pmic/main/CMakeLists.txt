idf_component_register(
    SRCS "pmic_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        candis_s31
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
