if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
  zephyr_library()
  zephyr_library_sources(${ZEPHYR_BASE}/misc/empty_file.c)
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_include_directories(${ZEPHYR_BASE}/lib/gui/lvgl/)
  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_CMAKE_DIR}/include)
  # Add include directory for fonts.h
  zephyr_library_include_directories(include)
  
  # Note: Board-specific overlay (boards/seeeduino_xiao_ble.overlay) automatically loaded
  # Contains conditional I2C/sensor configuration based on CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR
  # Main overlay provides safe defaults with I2C disabled
  
  # Essential scanner sources
  zephyr_library_sources(src/scanner_main.c)  # Message queue infrastructure for thread-safe architecture
  zephyr_library_sources(src/scanner_display.c)
  zephyr_library_sources(src/scanner_battery_widget.c)  # Battery widget for split keyboard display
  zephyr_library_sources(src/scanner_battery_status_widget.c)  # Scanner's own battery status widget
  zephyr_library_sources(src/connection_status_widget.c)  # YADS-style connection status widget
  zephyr_library_sources(src/layer_status_widget.c)  # YADS-style large layer display widget
  zephyr_library_sources(src/modifier_status_widget.c)  # Modifier key status widget (C S A G)
  # zephyr_library_sources(src/profile_status_widget.c)  # Removed - redundant with connection_status_widget
  # zephyr_library_sources(src/signal_status_widget.c)  # RSSI + reception rate widget - DISABLED (info in keyboard list)
  zephyr_library_sources(src/wpm_status_widget.c)  # YADS-style WPM display widget - RE-ENABLED
  # zephyr_library_sources(src/debug_status_widget.c)  # Debug status overlay widget - DISABLED (debug only)
  zephyr_library_sources(src/brightness_control.c)  # Auto-brightness with ambient light sensor

  # Touch-enabled version: Settings screens and touch handler
  if(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    zephyr_library_sources(src/system_settings_widget.c)  # Quick Actions screen (reset, bootloader)
    zephyr_library_sources(src/display_settings_widget.c)  # Display Settings screen (brightness, battery, layers)
    zephyr_library_sources(src/keyboard_list_widget.c)  # Keyboard list screen (shows active keyboards)
    zephyr_library_sources(src/behavior_settings_toggle.c)  # Custom behavior for settings toggle gesture
    zephyr_library_sources(src/touch_handler.c)  # Direct CST816S touch event handler for raw coordinates
    # Event system for thread-safe LVGL operations
    zephyr_library_sources(src/events/swipe_gesture_event.c)  # Swipe gesture event for touch input
  else()
    # Non-touch version: Signal status widget for display-only mode
    zephyr_library_sources(src/signal_status_widget.c)  # RSSI + reception rate widget
  endif()

  # zephyr_library_sources(src/display_rotate_init.c)  # Merged into scanner_display.c

  # NerdFont for modifier symbols (from YADS project - MIT License)
  zephyr_library_sources(src/fonts/NerdFonts_Regular_20.c)
  zephyr_library_sources(src/fonts/NerdFonts_Regular_40.c)
endif()