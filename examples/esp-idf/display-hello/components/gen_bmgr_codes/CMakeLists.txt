# Board information output
message(STATUS "Selected Board: candis_s31 (repository snapshot)")

# Board Manager's generated CMake normally compiles setup_device.c directly
# from the selected board package. Copy it into this component's build tree so
# ESP-IDF component validation sees one clear source owner while the vendor
# snapshot remains the source of truth.
set(CANDIS_BOARD_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../vendor/esp-board-manager/esp_friends_boards/candis_s31")
configure_file("${CANDIS_BOARD_DIR}/setup_device.c"
               "${CMAKE_CURRENT_BINARY_DIR}/candis_s31_setup_device.c"
               COPYONLY)

idf_component_register(
    SRC_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    INCLUDE_DIRS "."
    REQUIRES esp_board_manager esp_friends_boards esp_timer
)

# This is equivalent to adding WHOLE_ARCHIVE option to the idf_component_register call above:
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
