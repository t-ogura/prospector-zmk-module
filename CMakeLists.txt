cmake_minimum_required(VERSION 3.20.0)

# FORCE traditional approach to ensure BLE API linking works
# The modern zmk_library approach may not link BLE symbols correctly
zephyr_include_directories(include)
zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)

# Add status advertisement support - EXACTLY like rgbled-widget
if(CONFIG_ZMK_STATUS_ADVERTISEMENT)
        target_sources(app PRIVATE src/status_advertisement.c)
endif()

# Add scanner mode support
if(CONFIG_PROSPECTOR_MODE_SCANNER)
        target_sources(app PRIVATE src/status_scanner.c)

        # Get git describe for scanner version display
        execute_process(
                COMMAND git describe --tags --always
                WORKING_DIRECTORY ${ZEPHYR_CURRENT_MODULE_DIR}
                OUTPUT_VARIABLE PROSPECTOR_GIT_DESCRIBE
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE GIT_RESULT
        )
        if(GIT_RESULT EQUAL 0 AND PROSPECTOR_GIT_DESCRIBE)
                zephyr_compile_definitions(PROSPECTOR_GIT_VERSION="${PROSPECTOR_GIT_DESCRIBE}")
                message(STATUS "Prospector module version: ${PROSPECTOR_GIT_DESCRIBE}")
        endif()
endif()

# DISABLED FOR DEBUG: Custom display/LVGL drivers may be causing boot issues
# Display_test works without these - use Zephyr's built-in drivers instead
# if(CONFIG_SHIELD_PROSPECTOR_ADAPTER OR CONFIG_SHIELD_PROSPECTOR_SCANNER)
#         add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/drivers/display)
#         add_subdirectory(${ZEPHYR_CURRENT_MODULE_DIR}/modules/lvgl)
# endif()

if(CONFIG_SHIELD_PROSPECTOR_ADAPTER)
        if(CONFIG_DT_HAS_ZMK_BEHAVIOR_CAPS_WORD_ENABLED)
                target_sources(app PRIVATE src/events/caps_word_state_changed.c)
                set_source_files_properties(
                        ${APPLICATION_SOURCE_DIR}/src/behaviors/behavior_caps_word.c
                        TARGET_DIRECTORY app
                        PROPERTIES HEADER_FILE_ONLY ON)
                target_sources(app PRIVATE src/behaviors/behavior_caps_word.c)
        endif()

        target_sources(app PRIVATE src/events/split_central_status_changed.c)
        target_sources(app PRIVATE src/split/bluetooth/central_status_changed_observer.c)
endif()

# Scanner shield sources are handled by boards/shields/prospector_scanner/CMakeLists.txt