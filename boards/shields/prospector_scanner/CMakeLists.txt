# Prospector Scanner CMakeLists.txt
# Step-by-step restoration from display_test

if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
    # Add include directories for local headers
    target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

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

    # Brightness control with APDS9960 sensor for auto brightness
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/brightness_control.c)

    # System settings widget (Bootloader, Reset, Channel selector)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/system_settings_widget.c)

    # Prospector layouts interface (manages Operator/Radii switching)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/prospector_layouts.c)

    # Operator layout (carrefinho-inspired display style)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/operator_layout.c)

    # Radii layout (carrefinho-inspired wheel display style)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/radii_layout.c)

    # NerdFont for modifier icons
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts/NerdFonts_Regular_40.c)

    # Carrefinho fonts for Operator and Radii layouts
    # FG_Medium_20 - Modifiers, USB/BLE labels, slot labels
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts_carrefinho/FG_Medium_20.c)
    # FR_Medium_32 - WPM label
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts_carrefinho/FR_Medium_32.c)
    # DINish_Expanded_Light_36 - Layer name
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts_carrefinho/DINish_Expanded_Light_36.c)
    # DINish_Medium_24 - Battery labels
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts_carrefinho/DINish_Medium_24.c)
    # DINish_Condensed_Semibold_22 - Radii modifier labels (text mode, fallback)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts_carrefinho/DINish_Condensed_Semibold_22.c)
    # Symbols_Semibold_32 - Radii Mac-style modifier symbols (⌘, ⌥, ⌃, ⇧)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts_carrefinho/Symbols_Semibold_32.c)
endif()

# ORIGINAL FULL BUILD - TO BE RESTORED INCREMENTALLY
# if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
#   zephyr_library()
#   ... (original code preserved in comments)
# endif()
