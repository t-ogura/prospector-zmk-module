zephyr_include_directories(include)
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
zephyr_library()

if(CONFIG_SHIELD_PROSPECTOR_ADAPTER)

        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/drivers/display)
        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/modules/lvgl)

        if(CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED)
                zephyr_library_sources(src/events/caps_word_state_changed.c)
                set_source_files_properties(
                        ${APPLICATION_SOURCE_DIR}/src/behaviors/behavior_caps_word.c
                        TARGET_DIRECTORY app
                        PROPERTIES HEADER_FILE_ONLY ON)
                target_sources(app PRIVATE src/behaviors/behavior_caps_word.c)
        endif()

        zephyr_library_sources(src/events/split_central_status_changed.c)
        zephyr_library_sources(src/split/bluetooth/central_status_changed_observer.c)

endif()

# Add status advertisement support
if(CONFIG_ZMK_STATUS_ADVERTISEMENT)
        zephyr_library_sources(src/status_advertisement.c)
endif()

# Add scanner mode support
if(CONFIG_PROSPECTOR_MODE_SCANNER)
        zephyr_library_sources(src/status_scanner.c)
endif()

# Add scanner shield support
if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/drivers/display)
        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/modules/lvgl)
        zephyr_library_sources(boards/shields/prospector_scanner/src/scanner_display.c)
        zephyr_library_sources(boards/shields/prospector_scanner/src/brightness_control.c)
endif()