# Candis-S31 power-state cycle harness (measurement aid, not a product app).
cmake_minimum_required(VERSION 3.22)

# Same BSP wiring as firmware/demo: vendored snapshot by default, override
# with CANDIS_S31_BSP_PATH to point at a live esp-bsp checkout.
if(DEFINED ENV{CANDIS_S31_BSP_PATH} AND NOT "$ENV{CANDIS_S31_BSP_PATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{CANDIS_S31_BSP_PATH}" CANDIS_S31_BSP_PATH)
else()
    get_filename_component(CANDIS_S31_BSP_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/esp-bsp/bsp/candis_s31" ABSOLUTE)
endif()

if(NOT EXISTS "${CANDIS_S31_BSP_PATH}/CMakeLists.txt" OR
   NOT EXISTS "${CANDIS_S31_BSP_PATH}/include/bsp/candis_s31.h")
    message(FATAL_ERROR
        "Candis-S31 BSP not found at ${CANDIS_S31_BSP_PATH}: restore vendor/esp-bsp or set CANDIS_S31_BSP_PATH")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CANDIS_S31_BSP_PATH}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the application and its transitive BSP dependencies.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_powercycle)
