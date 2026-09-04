cmake_minimum_required(VERSION 3.22)

# Use the repository-owned board component and its local driver mappings.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/candis_components.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_usb_cdc)
