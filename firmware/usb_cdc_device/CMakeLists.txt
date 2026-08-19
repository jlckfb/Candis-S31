cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED ENV{CANDIS_S31_BSP_PATH} OR "$ENV{CANDIS_S31_BSP_PATH}" STREQUAL "")
    message(FATAL_ERROR
        "CANDIS_S31_BSP_PATH is required. Point it to esp-bsp/bsp/candis_s31.")
endif()

file(TO_CMAKE_PATH "$ENV{CANDIS_S31_BSP_PATH}" CANDIS_S31_BSP_PATH)
if(NOT EXISTS "${CANDIS_S31_BSP_PATH}/CMakeLists.txt" OR
   NOT EXISTS "${CANDIS_S31_BSP_PATH}/include/bsp/candis_s31.h")
    message(FATAL_ERROR
        "CANDIS_S31_BSP_PATH does not contain a Candis-S31 BSP: ${CANDIS_S31_BSP_PATH}")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CANDIS_S31_BSP_PATH}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_usb_cdc)
