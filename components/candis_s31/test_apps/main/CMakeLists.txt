idf_component_register(SRCS "test_bsp_storage.c"
                       INCLUDE_DIRS "."
                       REQUIRES candis_s31)

target_link_options(${COMPONENT_LIB} INTERFACE
                    "-Wl,--wrap=esp_vfs_spiffs_register"
                    "-Wl,--wrap=esp_spiffs_info"
                    "-Wl,--wrap=esp_vfs_spiffs_unregister"
                    "-u" "__wrap_esp_vfs_spiffs_register")
