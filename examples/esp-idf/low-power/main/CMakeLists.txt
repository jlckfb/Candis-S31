idf_component_register(
    SRCS
        "low_power_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        candis_s31
        esp_driver_gpio
        esp_lcd_touch_cst820
        rx8130ce
        ulp
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CANDIS_S31_BSP_GIT_REV="${CANDIS_S31_BSP_GIT_REV}"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

#
# LP core (ULP) support additions, following the IDF lp_core examples.
#
# 1. The LP Core app name must be unique across components.
set(ulp_app_name lp_core_${COMPONENT_NAME})
#
# 2. All LP core C sources. They live in lp_core/ and must NOT be part of the
#    component SRCS above: they are compiled by the LP toolchain instead.
set(ulp_lp_core_sources "lp_core/main.c")
#
# 3. Component sources that include the generated ${ulp_app_name}.h export
#    header, so CMake knows to rebuild them after the LP binary changes.
set(ulp_exp_dep_srcs "low_power_main.c")
#
# 4. Build the LP binary and embed it into the application. The component's
#    INCLUDE_DIRS are forwarded to the LP build, which is how lp_core/main.c
#    picks up lp_shared.h from this directory.
ulp_embed_binary(${ulp_app_name} "${ulp_lp_core_sources}" "${ulp_exp_dep_srcs}")
