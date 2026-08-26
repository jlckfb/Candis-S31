# The following lines must appear in this order for ESP-IDF projects.
cmake_minimum_required(VERSION 3.22)

# The Candis-S31 BSP defaults to the vendored snapshot in this repository
# (vendor/esp-bsp), so the project builds after a plain clone with only an
# ESP-IDF environment. Set CANDIS_S31_BSP_PATH to a live esp-bsp checkout
# (…/esp-bsp/bsp/candis_s31) to develop against BSP sources instead.
if(DEFINED ENV{CANDIS_S31_BSP_PATH} AND NOT "$ENV{CANDIS_S31_BSP_PATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{CANDIS_S31_BSP_PATH}" CANDIS_S31_BSP_PATH)
else()
    get_filename_component(CANDIS_S31_BSP_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/esp-bsp/bsp/candis_s31" ABSOLUTE)
endif()

if(NOT EXISTS "${CANDIS_S31_BSP_PATH}/CMakeLists.txt" OR
   NOT EXISTS "${CANDIS_S31_BSP_PATH}/include/bsp/candis_s31.h")
    message(FATAL_ERROR
        "Candis-S31 BSP not found at ${CANDIS_S31_BSP_PATH}: restore vendor/esp-bsp (tools/sync_bsp.sh) or set CANDIS_S31_BSP_PATH")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CANDIS_S31_BSP_PATH}")

if(CANDIS_S31_BSP_PATH MATCHES "/vendor/esp-bsp/")
    # Vendored snapshot: stamp from the sync metadata (the vendor tree is not
    # a separate git checkout).
    set(CANDIS_S31_BSP_GIT_REV "vendored")
    if(EXISTS "${CANDIS_S31_BSP_PATH}/../../SOURCE_COMMIT")
        file(STRINGS "${CANDIS_S31_BSP_PATH}/../../SOURCE_COMMIT" _bsp_src_meta)
        string(REGEX MATCH "commit: ([0-9a-f]+)" _match "${_bsp_src_meta}")
        if(CMAKE_MATCH_1)
            string(SUBSTRING "${CMAKE_MATCH_1}" 0 12 _bsp_src_short)
            set(CANDIS_S31_BSP_GIT_REV "vendored-${_bsp_src_short}")
        endif()
    endif()
else()
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
        OUTPUT_VARIABLE CANDIS_S31_BSP_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT CANDIS_S31_BSP_STATUS STREQUAL "")
        string(APPEND CANDIS_S31_BSP_GIT_REV "-dirty")
    endif()
endif()
set(CANDIS_S31_BSP_GIT_REV "${CANDIS_S31_BSP_GIT_REV}" CACHE INTERNAL "Candis-S31 BSP revision")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the application and its transitive BSP dependencies.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_player)
