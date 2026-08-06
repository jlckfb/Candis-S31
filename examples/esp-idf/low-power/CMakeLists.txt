# The following lines must appear in this order for ESP-IDF projects.
cmake_minimum_required(VERSION 3.22)

# The Candis-S31 BSP lives outside this repository, so its location is injected
# through the environment instead of being vendored. Same contract as
# firmware/factory/CMakeLists.txt.
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

execute_process(
    COMMAND git -C "${CANDIS_S31_BSP_PATH}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE CANDIS_S31_BSP_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(CANDIS_S31_BSP_GIT_REV STREQUAL "")
    set(CANDIS_S31_BSP_GIT_REV "unknown")
endif()
execute_process(
    COMMAND git -C "${CANDIS_S31_BSP_PATH}" status --porcelain --untracked-files=normal
    OUTPUT_VARIABLE CANDIS_S31_BSP_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CANDIS_S31_BSP_GIT_STATUS STREQUAL "")
    string(APPEND CANDIS_S31_BSP_GIT_REV "-dirty")
endif()
set(CANDIS_S31_BSP_GIT_REV "${CANDIS_S31_BSP_GIT_REV}" CACHE INTERNAL "Candis-S31 BSP revision")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the application and its transitive BSP dependencies.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_low_power)
