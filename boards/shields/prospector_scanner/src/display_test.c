/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_DISPLAY_TEST) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

static int display_test_init(void) {
    LOG_INF("=== DISPLAY TEST START ===");
    
    // Get display device
    const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    
    if (!display_dev) {
        LOG_ERR("Display device not found in device tree");
        return -ENODEV;
    }
    
    LOG_INF("Display device found: %s", display_dev->name);
    
    if (!device_is_ready(display_dev)) {
        LOG_ERR("Display device not ready");
        return -ENODEV;
    }
    
    LOG_INF("Display device is ready");
    
    // Get display capabilities
    struct display_capabilities caps;
    display_get_capabilities(display_dev, &caps);
    
    LOG_INF("Display capabilities:");
    LOG_INF("  Resolution: %dx%d", caps.x_resolution, caps.y_resolution);
    LOG_INF("  Pixel format: %d", caps.current_pixel_format);
    LOG_INF("  Screen info: %d", caps.screen_info);
    
    // Try to fill screen with a color
    LOG_INF("Attempting to fill screen with red...");
    
    // Create a simple buffer (red color)
    uint16_t buffer[240]; // One line of red pixels (RGB565 format)
    for (int i = 0; i < 240; i++) {
        buffer[i] = 0xF800; // Red in RGB565
    }
    
    // Fill the entire screen line by line
    struct display_buffer_descriptor desc = {
        .buf_size = sizeof(buffer),
        .width = 240,
        .height = 1,
        .pitch = 240,
    };
    
    int ret = 0;
    for (int y = 0; y < 280; y++) {
        ret = display_write(display_dev, 0, y, &desc, buffer);
        if (ret < 0) {
            LOG_ERR("Failed to write to display at line %d: %d", y, ret);
            break;
        }
    }
    
    if (ret >= 0) {
        LOG_INF("Successfully filled screen with red");
    }
    
    LOG_INF("=== DISPLAY TEST END ===");
    return 0;
}

SYS_INIT(display_test_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT + 10);

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY