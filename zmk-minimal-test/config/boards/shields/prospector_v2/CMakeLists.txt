# Prospector V2 CMakeLists.txt

if(CONFIG_SHIELD_PROSPECTOR_V2)
    target_sources(app PRIVATE backlight_init.c)
    target_sources_ifdef(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM app PRIVATE custom_status_screen.c)
endif()
