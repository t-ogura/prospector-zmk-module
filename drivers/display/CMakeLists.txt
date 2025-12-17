# Zephyr 4.1: Use standard ST7789V driver with MIPI DBI interface
# Custom driver disabled - requires Device Tree update for mipi-dbi-spi
# zephyr_library_amend()
# set_source_files_properties(
#         ${ZEPHYR_BASE}/drivers/display/display_st7789v.c
#         TARGET_DIRECTORY ${lib_name}
#         PROPERTIES HEADER_FILE_ONLY ON)
# zephyr_library_sources(display_st7789v.c)