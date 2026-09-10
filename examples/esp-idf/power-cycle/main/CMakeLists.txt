idf_component_register(
    SRCS "powercycle_main.c"
    INCLUDE_DIRS "."
    REQUIRES lwip candis_s31 tg28_sw
)
