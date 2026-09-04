cmake_minimum_required(VERSION 3.22)

# The generated board defaults are part of this checkout; loading them here
# keeps a plain clone independent of the optional bmgr action extension.
list(PREPEND SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/gen_bmgr_codes/board_manager.defaults")
# This example intentionally exercises the local Board Manager and friends
# board snapshots instead of the repository's advanced board runtime.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(candis_s31_display_hello)
