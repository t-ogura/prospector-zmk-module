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
  
  # Essential scanner sources
  zephyr_library_sources(src/scanner_display.c)
  zephyr_library_sources(src/scanner_battery_widget.c)  # Battery widget for split keyboard display
  zephyr_library_sources(src/connection_status_widget.c)  # YADS-style connection status widget
  zephyr_library_sources(src/layer_status_widget.c)  # YADS-style large layer display widget
  zephyr_library_sources(src/modifier_status_widget.c)  # Modifier key status widget (C S A G)
  # zephyr_library_sources(src/profile_status_widget.c)  # Removed - redundant with connection_status_widget
  zephyr_library_sources(src/signal_status_widget.c)  # RSSI + reception rate widget
  zephyr_library_sources(src/wpm_status_widget.c)  # YADS-style WPM display widget
  zephyr_library_sources(src/brightness_control.c)  # Auto-brightness with ambient light sensor
  # zephyr_library_sources(src/display_rotate_init.c)  # Merged into scanner_display.c
  
  # NerdFont for modifier symbols (from YADS project - MIT License)
  zephyr_library_sources(src/fonts/NerdFonts_Regular_20.c)
  zephyr_library_sources(src/fonts/NerdFonts_Regular_40.c)
endif()