# The following lines must appear in this order for ESP-IDF projects.
cmake_minimum_required(VERSION 3.22)

# The board implementation is first-party code in this repository. Its
# manifest maps reusable drivers to vendor/idf-extra-components.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/candis_components.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the application and its transitive board dependencies.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_demo)
