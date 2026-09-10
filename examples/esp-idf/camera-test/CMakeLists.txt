# Candis-S31 AMOLED camera preview demo.
cmake_minimum_required(VERSION 3.22)

# Use the repository-owned board component so the example builds from a plain
# clone without an external BSP checkout.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/candis_components.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Keep the image focused on the camera and display path; no audio, RF or other
# board service is initialised by the application.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_camera_test)
