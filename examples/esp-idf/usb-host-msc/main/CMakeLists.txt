idf_component_register(
    SRCS "usb_host_msc_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        candis_s31
        usb_host_msc
        esp_timer
        fatfs
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
