# Candis-S31 power-state cycle harness (measurement aid, not a product app).
cmake_minimum_required(VERSION 3.22)

# Use the repository-owned board component. Reusable drivers are mapped by its
# manifest to vendor/idf-extra-components.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/candis_components.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the application and its transitive board dependencies.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_powercycle)
