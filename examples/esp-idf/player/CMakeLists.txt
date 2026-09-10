# The following lines must appear in this order for ESP-IDF projects.
cmake_minimum_required(VERSION 3.22)

# Use the repository-owned board component. The component manifest maps the
# four reusable drivers to vendor/idf-extra-components.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/candis_components.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(candis_s31_player)
