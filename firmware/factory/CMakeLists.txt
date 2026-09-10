cmake_minimum_required(VERSION 3.22)

# The Candis-S31 board implementation is maintained in this repository.
# Reusable chip drivers are resolved from vendor/idf-extra-components by the
# board component manifest.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/candis_components.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the application and its transitive board dependencies.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_factory)
