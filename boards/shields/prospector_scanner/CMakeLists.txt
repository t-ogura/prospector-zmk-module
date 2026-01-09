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

    # Brightness control with APDS9960 sensor for auto brightness
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/brightness_control.c)

    # NerdFont for modifier icons
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE src/fonts/NerdFonts_Regular_40.c)
endif()

# ORIGINAL FULL BUILD - TO BE RESTORED INCREMENTALLY
# if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
#   zephyr_library()
#   ... (original code preserved in comments)
# endif()
