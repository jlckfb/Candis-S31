# Candis-S31 headless camera driver diagnostic.
cmake_minimum_required(VERSION 3.22)

# Use the repository-owned board component. This keeps the camera diagnostic
# independent of any external BSP checkout.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/candis_components.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Keep the image focused on the camera path; no display, audio, RF, or UI
# service is initialized by the application.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_camera_test)
