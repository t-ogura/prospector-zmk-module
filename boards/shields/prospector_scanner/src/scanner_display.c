/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

// Display rotation initialization (merged from display_rotate_init.c)
static int scanner_display_init(void) {
    LOG_INF("Initializing scanner display system");
    
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display)) {
        LOG_ERR("Display device not ready");
        return -EIO;
    }
    
    // Set display orientation
#ifdef CONFIG_PROSPECTOR_ROTATE_DISPLAY_180
    int ret = display_set_orientation(display, DISPLAY_ORIENTATION_ROTATED_90);
#else
    int ret = display_set_orientation(display, DISPLAY_ORIENTATION_ROTATED_270);
#endif
    if (ret < 0) {
        LOG_ERR("Failed to set display orientation: %d", ret);
        return ret;
    }
    
    // Ensure display stays on and disable blanking
    ret = display_blanking_off(display);
    if (ret < 0) {
        LOG_WRN("Failed to turn off display blanking: %d", ret);
    }
    
    // Force backlight on using GPIO (fallback method)
    const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    if (device_is_ready(gpio_dev)) {
        ret = gpio_pin_configure(gpio_dev, 11, GPIO_OUTPUT_ACTIVE);
        if (ret == 0) {
            gpio_pin_set(gpio_dev, 11, 1);  // Turn on backlight
            LOG_INF("Backlight GPIO set to ON");
        } else {
            LOG_WRN("Failed to configure backlight GPIO: %d", ret);
        }
    }
    
    // Add a delay to allow display to stabilize
    k_msleep(100);
    
    LOG_INF("Scanner display initialized successfully");
    return 0;
}

// Initialize display early in the boot process
SYS_INIT(scanner_display_init, APPLICATION, 60);

// Required function for ZMK_DISPLAY_STATUS_SCREEN_CUSTOM
// Following the working adapter pattern with simple, stable display
lv_obj_t *zmk_display_status_screen() {
    LOG_INF("Creating scanner status screen");
    
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);
    
    // Main title
    lv_obj_t *title_label = lv_label_create(screen);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);
    lv_label_set_text(title_label, "Prospector Scanner");
    
    // Status label
    lv_obj_t *status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_label, lv_color_make(255, 255, 0), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(status_label, "Scanning...");
    
    // Info label
    lv_obj_t *info_label = lv_label_create(screen);
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_20, 0);
    lv_obj_align(info_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text(info_label, "Ready for keyboards");
    
    LOG_INF("Scanner screen created successfully");
    return screen;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY