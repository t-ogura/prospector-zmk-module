# Zephyr 4.1 / LVGL 9: Use standard Zephyr LVGL integration
# Custom lvgl.c disabled - LVGL 8 API not compatible with LVGL 9
# find_package(Zephyr)
# zephyr_library_amend()
# set_source_files_properties(
#         ${ZEPHYR_BASE}/modules/lvgl/lvgl.c
#         TARGET_DIRECTORY ${lib_name}
#         PROPERTIES HEADER_FILE_ONLY ON)
# zephyr_library_sources(lvgl.c)