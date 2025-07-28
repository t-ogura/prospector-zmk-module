if(CONFIG_SHIELD_PROSPECTOR_SCANNER)
  zephyr_library()
  zephyr_library_sources(${ZEPHYR_BASE}/misc/empty_file.c)
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_include_directories(${ZEPHYR_BASE}/lib/gui/lvgl/)
  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_CMAKE_DIR}/include)
  
  # Essential scanner sources
  zephyr_library_sources(src/scanner_display.c)
  zephyr_library_sources(src/scanner_battery_widget.c)  # Battery widget for split keyboard display
  # zephyr_library_sources(src/connection_status_widget.c)  # YADS-style connection status widget - temporarily disabled
  zephyr_library_sources(src/brightness_control.c)  # Re-enabled with PWM control
  # zephyr_library_sources(src/display_rotate_init.c)  # Merged into scanner_display.c
endif()