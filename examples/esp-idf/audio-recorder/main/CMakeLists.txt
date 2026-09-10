idf_component_register(
    SRCS "audio_recorder_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        example_board
        candis_s31
        fatfs
        esp_codec_dev
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
