idf_component_register(
    SRCS "ble_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        candis_s31
        bt
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
