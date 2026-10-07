# SPDX-License-Identifier: MIT
#
# Reuses the v1 Scanner Pocket display sources in place - no copies.

if(CONFIG_SHIELD_SCANNER_POCKET_V2)
  set(V1 ${CMAKE_CURRENT_SOURCE_DIR}/../scanner_pocket)

  zephyr_library()
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)
  zephyr_library_include_directories(${V1}/assets)

  zephyr_library_sources(${V1}/assets/quinquefive_8.c)
  zephyr_library_sources(${V1}/assets/quinquefive_12.c)
  zephyr_library_sources(${V1}/assets/quinquefive_24.c)
  zephyr_library_sources(${V1}/assets/unscii_14.c)
  zephyr_library_sources(${V1}/assets/NerdFonts_Regular_20.c)
  zephyr_library_sources(${V1}/assets/NerdFonts_Regular_40.c)

  zephyr_library_sources(${V1}/src/pocket_display.c)

  if(CONFIG_SCANNER_POCKET_LANDSCAPE)
    zephyr_library_sources(${V1}/src/ls0xx_rotated.c)
  endif()
endif()
