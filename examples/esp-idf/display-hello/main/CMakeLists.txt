idf_component_register(
    SRCS
        "display_hello_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_board_manager
        esp_lvgl_adapter
        lvgl
        gen_bmgr_codes
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
