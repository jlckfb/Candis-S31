idf_component_register(
    SRCS "wifi_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        candis_s31
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
