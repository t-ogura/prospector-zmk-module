# Display Test CMakeLists.txt

if(CONFIG_SHIELD_DISPLAY_TEST)
    target_sources(app PRIVATE backlight_init.c)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE custom_status_screen.c)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE hello_widget.c)
    # Scanner stub for linking with status_scanner.c
    target_sources_ifdef(CONFIG_PROSPECTOR_MODE_SCANNER app PRIVATE scanner_stub.c)
    # Touch input handler with swipe events
    target_sources_ifdef(CONFIG_INPUT_CST816S app PRIVATE touch_input_handler.c)
    target_sources_ifdef(CONFIG_INPUT_CST816S app PRIVATE swipe_event.c)
    # Light sensor handler
    target_sources_ifdef(CONFIG_APDS9960 app PRIVATE light_sensor_handler.c)
endif()
