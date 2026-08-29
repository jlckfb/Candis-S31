cmake_minimum_required(VERSION 3.16)
project(candis_s31_lvgl_sim C)

# Keep the simulator tied to the exact LVGL tree used by the firmware. The
# managed component is intentionally ignored by Git, so prepare_lvgl.sh must
# populate it before a fresh-clone build.
set(CANDIS_LVGL_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/lvgl__lvgl"
    CACHE PATH "LVGL source tree used by firmware/demo")
set(CANDIS_DEMO_MAIN_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../main"
    CACHE PATH "firmware/demo/main source tree")

if(NOT EXISTS "${CANDIS_LVGL_ROOT}/lv_version.h")
    message(FATAL_ERROR
        "LVGL source not found at ${CANDIS_LVGL_ROOT}. Run "
        "${CMAKE_CURRENT_LIST_DIR}/prepare_lvgl.sh first, or set "
        "-DCANDIS_LVGL_ROOT=<firmware/demo/managed_components/lvgl__lvgl>.")
endif()

file(READ "${CANDIS_LVGL_ROOT}/lv_version.h" CANDIS_LVGL_VERSION_H)
string(REGEX MATCH
    "LVGL_VERSION_MAJOR[ \t]+([0-9]+)" _major_match
    "${CANDIS_LVGL_VERSION_H}")
set(CANDIS_LVGL_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH
    "LVGL_VERSION_MINOR[ \t]+([0-9]+)" _minor_match
    "${CANDIS_LVGL_VERSION_H}")
set(CANDIS_LVGL_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH
    "LVGL_VERSION_PATCH[ \t]+([0-9]+)" _patch_match
    "${CANDIS_LVGL_VERSION_H}")
set(CANDIS_LVGL_PATCH "${CMAKE_MATCH_1}")
if(NOT CANDIS_LVGL_MAJOR STREQUAL "9" OR
   NOT CANDIS_LVGL_MINOR STREQUAL "5" OR
   NOT CANDIS_LVGL_PATCH STREQUAL "0")
    message(FATAL_ERROR
        "Candis-S31 simulator requires the firmware LVGL 9.5.0 tree; "
        "found ${CANDIS_LVGL_MAJOR}.${CANDIS_LVGL_MINOR}.${CANDIS_LVGL_PATCH} "
        "at ${CANDIS_LVGL_ROOT}")
endif()
message(STATUS "Using firmware LVGL ${CANDIS_LVGL_MAJOR}.${CANDIS_LVGL_MINOR}.${CANDIS_LVGL_PATCH}")

set(CANDIS_LVGL_EXPECTED_HASH
    "184e532558c1c45fefed631f3e235423d22582aafb4630f3e8885c35281a49ae")
if(EXISTS "${CANDIS_LVGL_ROOT}/.component_hash")
    file(STRINGS "${CANDIS_LVGL_ROOT}/.component_hash" CANDIS_LVGL_HASH)
    string(STRIP "${CANDIS_LVGL_HASH}" CANDIS_LVGL_HASH)
    if(NOT CANDIS_LVGL_HASH STREQUAL CANDIS_LVGL_EXPECTED_HASH)
        message(FATAL_ERROR
            "LVGL component hash differs from the firmware lock: "
            "${CANDIS_LVGL_HASH}")
    endif()
else()
    message(FATAL_ERROR
        "Missing .component_hash in ${CANDIS_LVGL_ROOT}; restore the "
        "managed component with sim/prepare_lvgl.sh")
endif()
message(STATUS "Using firmware LVGL component hash ${CANDIS_LVGL_HASH}")

# os_desktop.cmake consumes LV_BUILD_CONF_PATH when ESP_PLATFORM is absent.
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE FILEPATH
    "LVGL host configuration" FORCE)
set(LV_BUILD_LVGL_H_SIMPLE_INCLUDE ON CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

if(EMSCRIPTEN)
    # The FreeRTOS shim runs services/tests on real threads; Emscripten
    # pthreads need SharedArrayBuffer, which the preview server exposes via
    # COOP/COEP headers (sim/serve_web.sh). Must be set before any target
    # (including the lvgl subdirectory) so every object is compiled with
    # atomics/bulk-memory enabled.
    add_compile_options("-sUSE_PTHREADS")
endif()

find_package(Threads REQUIRED)
if(NOT EMSCRIPTEN)
    find_package(SDL2 CONFIG QUIET)
    if(NOT TARGET SDL2::SDL2)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        add_library(candis_sdl2 INTERFACE)
        target_link_libraries(candis_sdl2 INTERFACE PkgConfig::SDL2)
        set(CANDIS_SDL_TARGET candis_sdl2)
    else()
        set(CANDIS_SDL_TARGET SDL2::SDL2)
    endif()
else()
    # Emscripten supplies SDL2 through its ports system at link time.
    set(CANDIS_SDL_TARGET "")
endif()
add_subdirectory("${CANDIS_LVGL_ROOT}" "${CMAKE_BINARY_DIR}/lvgl" EXCLUDE_FROM_ALL)
if(EMSCRIPTEN)
    target_compile_options(lvgl PRIVATE "-sUSE_SDL=2")
else()
    target_link_libraries(lvgl PRIVATE ${CANDIS_SDL_TARGET})
endif()

set(CANDIS_SIM_SOURCES
    main/sim_main.c
    main/sim_ui.c
    main/sim_app_registry.c
    main/sim_tests.c
    compat/sim_compat.c
    compat/esp_sim.c
    compat/freertos_sim.c
    compat/services/svc_net_sim.c
    compat/services/svc_audio_sim.c
    compat/services/svc_storage_sim.c
    compat/tests/led_hw_sim.c
    compat/tests/display_diag_sim.c
    "${CANDIS_DEMO_MAIN_ROOT}/ui/ui_manager.c"
    "${CANDIS_DEMO_MAIN_ROOT}/ui/ui_menu.c"
    "${CANDIS_DEMO_MAIN_ROOT}/ui/ui_watchface.c"
    "${CANDIS_DEMO_MAIN_ROOT}/ui/ui_keyboard.c"
    "${CANDIS_DEMO_MAIN_ROOT}/ui/ui_widgets.c"
    "${CANDIS_DEMO_MAIN_ROOT}/ui/ui_perf.c"
    "${CANDIS_DEMO_MAIN_ROOT}/ui/fonts/candis_ui_20.c"
    "${CANDIS_DEMO_MAIN_ROOT}/ui/fonts/candis_ui_24.c"
    "${CANDIS_DEMO_MAIN_ROOT}/games/game_2048.c"
    "${CANDIS_DEMO_MAIN_ROOT}/games/game_snake.c"
    "${CANDIS_DEMO_MAIN_ROOT}/games/game_breakout.c"
    # Real app pages (camera/usb stay BOARD ONLY: no V4L2/USB-host model).
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_test_center.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_test_run.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_display_test.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_recorder.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_player.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_wifi.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_ble.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_files.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_led.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_settings.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_power.c"
    "${CANDIS_DEMO_MAIN_ROOT}/apps/app_sysinfo.c"
    # Real test framework (registry/runner/report) with sim stub suites.
    "${CANDIS_DEMO_MAIN_ROOT}/tests/test_registry.c"
    "${CANDIS_DEMO_MAIN_ROOT}/tests/test_report.c"
    "${CANDIS_DEMO_MAIN_ROOT}/tests/svc_test.c"
)

add_executable(candis_s31_sim ${CANDIS_SIM_SOURCES})
target_include_directories(candis_s31_sim PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/compat"
    "${CANDIS_DEMO_MAIN_ROOT}"
    "${CANDIS_DEMO_MAIN_ROOT}/tests"
    "${CANDIS_LVGL_ROOT}"
)
target_compile_definitions(candis_s31_sim PRIVATE
    CANDIS_S31_SIM=1
    LV_CONF_INCLUDE_SIMPLE=1
    CANDIS_S31_BSP_GIT_REV="sim-host"
    CONFIG_IDF_TARGET="esp32s31"
)
target_compile_options(candis_s31_sim PRIVATE -Wall -Wextra)
target_link_libraries(candis_s31_sim PRIVATE lvgl ${CANDIS_SDL_TARGET} Threads::Threads m)

if(EMSCRIPTEN)
    set_target_properties(candis_s31_sim PROPERTIES SUFFIX ".html")
    target_link_options(candis_s31_sim PRIVATE
        "-sUSE_SDL=2"
        "-sUSE_PTHREADS"
        "-sPTHREAD_POOL_SIZE=8"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=67108864"
        "-sMINIFY_HTML=0"
        "--shell-file=${CMAKE_CURRENT_LIST_DIR}/web/candis_shell.html")
endif()

add_custom_target(run
    COMMAND candis_s31_sim
    DEPENDS candis_s31_sim
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL)
