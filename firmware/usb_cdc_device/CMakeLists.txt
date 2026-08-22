cmake_minimum_required(VERSION 3.22)

# The Candis-S31 BSP defaults to the vendored snapshot in this repository
# (vendor/esp-bsp), so the project builds after a plain clone with only an
# ESP-IDF environment. Set CANDIS_S31_BSP_PATH to a live esp-bsp checkout
# (…/esp-bsp/bsp/candis_s31) to develop against BSP sources instead.
if(DEFINED ENV{CANDIS_S31_BSP_PATH} AND NOT "$ENV{CANDIS_S31_BSP_PATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{CANDIS_S31_BSP_PATH}" CANDIS_S31_BSP_PATH)
else()
    get_filename_component(CANDIS_S31_BSP_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/esp-bsp/bsp/candis_s31" ABSOLUTE)
endif()

if(NOT EXISTS "${CANDIS_S31_BSP_PATH}/CMakeLists.txt" OR
   NOT EXISTS "${CANDIS_S31_BSP_PATH}/include/bsp/candis_s31.h")
    message(FATAL_ERROR
        "Candis-S31 BSP not found at ${CANDIS_S31_BSP_PATH}: restore vendor/esp-bsp (tools/sync_bsp.sh) or set CANDIS_S31_BSP_PATH")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CANDIS_S31_BSP_PATH}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_usb_cdc)
