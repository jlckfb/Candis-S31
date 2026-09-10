# Candis-S31 RTC example: RX8130CE time read/set plus alarm over the shared IRQ line.
cmake_minimum_required(VERSION 3.22)

# Use the repository-owned board component. The component manifest maps the
# four reusable drivers to vendor/idf-extra-components.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/candis_components.cmake")

# Shared example bootstrap (board init, physical input).
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

set(COMPONENTS main)
set(MINIMAL_BUILD ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(candis_s31_rtc)
