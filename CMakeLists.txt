cmake_minimum_required(VERSION 3.20.0)

# Check if zmk_library is available
if(COMMAND zmk_library)
    # Modern ZMK approach using zmk_library
    if(CONFIG_ZMK_STATUS_ADVERTISEMENT)
        zmk_library(
            NAME prospector_status_adv
            SRCS src/status_advertisement.c
            DEPS
                ble         # zmk_ble_active_profile_index(), etc.
                usb         # zmk_usb_is_hid_ready(), etc.
                battery     # zmk_battery_state_of_charge()
                hid         # zmk_hid_get_keyboard_report()
                endpoints
        )
    endif()
    
    if(CONFIG_PROSPECTOR_MODE_SCANNER)
        zmk_library(
            NAME prospector_scanner
            SRCS src/status_scanner.c
        )
    endif()
else()
    # Fallback to traditional approach
    zephyr_include_directories(include)
    zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
    zephyr_library()
    
    # Add status advertisement support
    if(CONFIG_ZMK_STATUS_ADVERTISEMENT)
            zephyr_library_sources(src/status_advertisement.c)
    endif()
    
    # Add scanner mode support
    if(CONFIG_PROSPECTOR_MODE_SCANNER)
            zephyr_library_sources(src/status_scanner.c)
    endif()
endif()

# Add display and LVGL support once for any shield that needs it
if(CONFIG_SHIELD_PROSPECTOR_ADAPTER OR CONFIG_SHIELD_PROSPECTOR_SCANNER)
        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/drivers/display)
        add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/modules/lvgl)
endif()

if(CONFIG_SHIELD_PROSPECTOR_ADAPTER)
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

# Scanner shield sources are handled by boards/shields/prospector_scanner/CMakeLists.txt