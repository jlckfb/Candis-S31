# Shared component wiring for standalone ESP-IDF projects in this repository.
# The board implementation belongs to this repository; reusable chip drivers
# are resolved from vendor/idf-extra-components by its component manifest.

if(DEFINED ENV{CANDIS_COMPONENTS_ROOT} AND NOT "$ENV{CANDIS_COMPONENTS_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{CANDIS_COMPONENTS_ROOT}" CANDIS_COMPONENTS_ROOT)
else()
    get_filename_component(CANDIS_COMPONENTS_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../components" ABSOLUTE)
endif()

set(CANDIS_BOARD_COMPONENT_DIR "${CANDIS_COMPONENTS_ROOT}/candis_s31")
if(NOT EXISTS "${CANDIS_BOARD_COMPONENT_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${CANDIS_BOARD_COMPONENT_DIR}/include/bsp/candis_s31.h")
    message(FATAL_ERROR
        "Candis-S31 board component not found at ${CANDIS_BOARD_COMPONENT_DIR}")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CANDIS_BOARD_COMPONENT_DIR}")
set(CANDIS_S31_BSP_GIT_REV "repository-board-component" CACHE INTERNAL "Candis-S31 board component revision")
