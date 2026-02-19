# Prospector Scanner CMakeLists.txt
# Step-by-step restoration from display_test

if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
    # Backlight initialization (always needed)
    target_sources(app PRIVATE src/backlight_init.c)

    # CUSTOM screen support (from display_test)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/custom_status_screen.c)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/hello_widget.c)

    # Swipe gesture event (ZMK event system - thread-safe)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/events/swipe_gesture_event.c)

    # Touch handler (when touch is enabled)
    target_sources_ifdef(CONFIG_PROSPECTOR_TOUCH_ENABLED app PRIVATE src/touch_handler.c)

    # Scanner stub for message handling (required by custom_status_screen.c)
    target_sources_ifdef(CONFIG_PROSPECTOR_MODE_SCANNER app PRIVATE src/scanner_stub.c)

    # Display settings persistence (NVS)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/display_settings.c)

    # Brightness control with APDS9960 sensor for auto brightness
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/brightness_control.c)

    # System settings widget (Bootloader, Reset, Channel selector)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/system_settings_widget.c)

    # NerdFont for modifier icons
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts/NerdFonts_Regular_40.c)

    # Prospector Display layouts (Carrefinho-inspired)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/prospector_layouts.c)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/field_layout.c)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/operator_layout.c)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/radii_layout.c)

    # Carrefinho custom fonts
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE
        src/fonts_carrefinho/DINish_Condensed_Semibold_20.c
        src/fonts_carrefinho/DINish_Condensed_Semibold_22.c
        src/fonts_carrefinho/DINish_Expanded_Light_36.c
        src/fonts_carrefinho/DINish_Medium_24.c
        src/fonts_carrefinho/FG_Medium_20.c
        src/fonts_carrefinho/FG_Medium_21.c
        src/fonts_carrefinho/FG_Medium_24.c
        src/fonts_carrefinho/FG_Medium_26.c
        src/fonts_carrefinho/FR_Medium_32.c
        src/fonts_carrefinho/FR_Regular_30.c
        src/fonts_carrefinho/FR_Regular_36.c
        src/fonts_carrefinho/FR_Regular_48.c
        src/fonts_carrefinho/FR_Thin_48.c
        src/fonts_carrefinho/PPF_NarrowThin_64.c
        src/fonts_carrefinho/Symbols_Bold_26.c
        src/fonts_carrefinho/Symbols_Medium_28.c
        src/fonts_carrefinho/Symbols_Regular_28.c
        src/fonts_carrefinho/Symbols_Semibold_28.c
        src/fonts_carrefinho/Symbols_Semibold_32.c
    )

    # Include path for local headers
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# ORIGINAL FULL BUILD - TO BE RESTORED INCREMENTALLY
# if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
#   zephyr_library()
#   ... (original code preserved in comments)
# endif()
