idf_component_register(
    SRCS "example_board.c"
    INCLUDE_DIRS "include"
    REQUIRES
        candis_s31
        nvs_flash
        esp_psram
        heap
        freertos
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
