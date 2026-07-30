# The following lines must appear in this order for ESP-IDF projects.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components required by this application.
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_getting_started)
