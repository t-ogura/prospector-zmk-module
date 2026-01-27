# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_SHIELD_SCANNER_POCKET)
  zephyr_library()
  zephyr_library_sources(${ZEPHYR_BASE}/misc/empty_file.c)
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_include_directories(${ZEPHYR_BASE}/lib/gui/lvgl/)
  zephyr_library_include_directories(${ZEPHYR_BASE}/drivers)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
  zephyr_library_include_directories(${ZEPHYR_CURRENT_CMAKE_DIR}/include)

  # Add include directory for local headers
  zephyr_library_include_directories(include)

  # Scanner Pocket core
  zephyr_library_sources(src/pocket_display.c)
  zephyr_library_sources(src/scanner_stub.c)

  # Rotated display driver for landscape mode
  if(CONFIG_SCANNER_POCKET_LANDSCAPE)
    zephyr_library_sources(src/ls0xx_rotated.c)
  endif()

  # TODO: Add widgets as they are implemented
  # zephyr_library_sources(src/widgets/keyboard_widget.c)
  # zephyr_library_sources(src/widgets/layer_widget.c)
  # zephyr_library_sources(src/widgets/battery_widget.c)
  # zephyr_library_sources(src/widgets/signal_widget.c)
endif()
