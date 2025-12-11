# Custom LVGL glue code disabled - using Zephyr standard LVGL 9 integration
# The custom lvgl.c was for LVGL 8, not compatible with LVGL 9
# To restore, uncomment:
# find_package(Zephyr)
# zephyr_library_amend()
# set_source_files_properties(
#         ${ZEPHYR_BASE}/modules/lvgl/lvgl.c
#         TARGET_DIRECTORY ${lib_name}
#         PROPERTIES HEADER_FILE_ONLY ON)
# zephyr_library_sources(lvgl.c)
