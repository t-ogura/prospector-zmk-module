# Custom ST7789V driver disabled - using Zephyr standard driver with MIPI DBI
# To restore custom driver, uncomment the following:
# zephyr_library_amend()
# set_source_files_properties(
#         ${ZEPHYR_BASE}/drivers/display/display_st7789v.c
#         TARGET_DIRECTORY ${lib_name}
#         PROPERTIES HEADER_FILE_ONLY ON)
# zephyr_library_sources(display_st7789v.c)
