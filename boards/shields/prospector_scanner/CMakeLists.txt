# Prospector Scanner Shield

if(CONFIG_PROSPECTOR_MODE_SCANNER)
    target_sources(app PRIVATE 
        src/scanner_display.c
        src/brightness_control.c
        src/display_test.c
    )
endif()