/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zmk/status_scanner.h>
#include "scanner_battery_widget.h"
// #include "connection_status_widget.h" // Temporarily disabled for debugging

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

// Global UI objects for dynamic updates
static lv_obj_t *status_label = NULL;
static lv_obj_t *info_label = NULL;
static struct zmk_widget_scanner_battery battery_widget;
// static struct zmk_widget_connection_status connection_widget; // Temporarily disabled

// Forward declaration
static void trigger_scanner_start(void);

// Scanner event callback for display updates
static void update_display_from_scanner(struct zmk_status_scanner_event_data *event_data) {
    if (!status_label || !info_label) {
        return; // UI not ready yet
    }
    
    LOG_INF("Scanner event received: %d for keyboard %d", event_data->event, event_data->keyboard_index);
    
    int active_count = zmk_status_scanner_get_active_count();
    
    if (active_count == 0) {
        // No keyboards found
        lv_label_set_text(status_label, "Scanning...");
        lv_label_set_text(info_label, "No keyboards found");
        LOG_INF("Display updated: No keyboards");
    } else {
        // Debug: show active count
        char debug_buf[32];
        snprintf(debug_buf, sizeof(debug_buf), "Found %d devices", active_count);
        // Find active keyboards and display their info  
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
            if (kbd && kbd->active) {
                // Check if this is a split keyboard (Central with peripheral battery data)
                if (kbd->data.device_role == ZMK_DEVICE_ROLE_CENTRAL && 
                    kbd->data.peripheral_battery[0] > 0) {
                    // Split keyboard - Central contains both battery levels
                    lv_label_set_text(status_label, kbd->ble_name);
                    char info_buf[64];
                    snprintf(info_buf, sizeof(info_buf), "Layer %d | R:%d%% L:%d%%", 
                            kbd->data.active_layer, 
                            kbd->data.battery_level,           // Central (Right)
                            kbd->data.peripheral_battery[0]);  // Peripheral (Left)
                    lv_label_set_text(info_label, info_buf);
                    
                    zmk_widget_scanner_battery_update(&battery_widget, kbd);
                    // zmk_widget_connection_status_update(&connection_widget, kbd);
                    
                    LOG_INF("Split keyboard: Central %d%%, Peripheral %d%%, Layer: %d", 
                            kbd->data.battery_level, kbd->data.peripheral_battery[0], kbd->data.active_layer);
                } else if (kbd->data.device_role == ZMK_DEVICE_ROLE_CENTRAL) {
                    // Central device without peripheral data
                    lv_label_set_text(status_label, kbd->ble_name);
                    char info_buf[64];
                    snprintf(info_buf, sizeof(info_buf), "Layer %d: %d%%", 
                            kbd->data.active_layer, kbd->data.battery_level);
                    lv_label_set_text(info_label, info_buf);
                    
                    zmk_widget_scanner_battery_update(&battery_widget, kbd);
                    // zmk_widget_connection_status_update(&connection_widget, kbd);
                    
                    LOG_INF("Central device: %d%%, Layer: %d", 
                            kbd->data.battery_level, kbd->data.active_layer);
                } else if (kbd->data.device_role == ZMK_DEVICE_ROLE_STANDALONE) {
                    // Standalone keyboard
                    lv_label_set_text(status_label, kbd->ble_name);
                    char info_buf[64];
                    snprintf(info_buf, sizeof(info_buf), "Layer %d: %d%%", 
                            kbd->data.active_layer, kbd->data.battery_level);
                    lv_label_set_text(info_label, info_buf);
                    
                    zmk_widget_scanner_battery_update(&battery_widget, kbd);
                    // zmk_widget_connection_status_update(&connection_widget, kbd);
                    
                    LOG_INF("Standalone keyboard: %d%%, Layer: %d", 
                            kbd->data.battery_level, kbd->data.active_layer);
                } else {
                    // Peripheral device (shouldn't normally happen with new implementation)
                    lv_label_set_text(status_label, debug_buf);
                    char info_buf[64];
                    snprintf(info_buf, sizeof(info_buf), "Peripheral L%d: %d%%", 
                            kbd->data.active_layer, kbd->data.battery_level);
                    lv_label_set_text(info_label, info_buf);
                    
                    zmk_widget_scanner_battery_update(&battery_widget, kbd);
                    // zmk_widget_connection_status_update(&connection_widget, kbd);
                    
                    LOG_INF("Peripheral device: %d%%, Layer: %d", 
                            kbd->data.battery_level, kbd->data.active_layer);
                }
                break; // Only handle the first active keyboard for now
            }
        }
    }
}

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
    
    // Note: Backlight control is now handled by brightness_control.c
    
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
    
    // Status label (save reference for updates) - moved higher without title
    status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_label, lv_color_make(255, 255, 0), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -60);
    lv_label_set_text(status_label, "Initializing...");
    
    // Battery widget in the center
    zmk_widget_scanner_battery_init(&battery_widget, screen);
    lv_obj_align(zmk_widget_scanner_battery_obj(&battery_widget), LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_height(zmk_widget_scanner_battery_obj(&battery_widget), 60);
    
    // Connection status widget in top right - temporarily disabled for debugging
    // zmk_widget_connection_status_init(&connection_widget, screen);
    // lv_obj_align(zmk_widget_connection_status_obj(&connection_widget), LV_ALIGN_TOP_RIGHT, -5, 35);
    
    // Info label (save reference for updates)
    info_label = lv_label_create(screen);
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_12, 0);
    lv_obj_align(info_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(info_label, "Starting scanner...");
    
    // Trigger scanner initialization after screen is ready
    trigger_scanner_start();
    
    LOG_INF("Scanner screen created successfully");
    return screen;
}

// Late initialization to start scanner after display is ready
static void start_scanner_delayed(struct k_work *work) {
    if (!status_label || !info_label) {
        LOG_WRN("Display not ready yet, retrying scanner start...");
        k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(1));
        return;
    }
    
    LOG_INF("Starting BLE scanner...");
    lv_label_set_text(status_label, "Starting scanner...");
    lv_label_set_text(info_label, "Initializing BLE...");
    
    // Register callback first
    int ret = zmk_status_scanner_register_callback(update_display_from_scanner);
    if (ret < 0) {
        LOG_ERR("Failed to register scanner callback: %d", ret);
        lv_label_set_text(status_label, "Scanner Error");
        lv_label_set_text(info_label, "Callback failed");
        return;
    }
    
    // Start scanning
    ret = zmk_status_scanner_start();
    if (ret < 0) {
        LOG_ERR("Failed to start scanner: %d", ret);
        lv_label_set_text(status_label, "Scanner Error");
        lv_label_set_text(info_label, "Start failed");
        return;
    }
    
    LOG_INF("BLE scanner started successfully");
    lv_label_set_text(status_label, "Scanning...");
    lv_label_set_text(info_label, "Ready for keyboards");
}

static K_WORK_DELAYABLE_DEFINE(scanner_start_work, start_scanner_delayed);

// Trigger scanner start automatically when screen is created
static void trigger_scanner_start(void) {
    LOG_INF("Scheduling delayed scanner start from display creation");
    k_work_schedule(&scanner_start_work, K_SECONDS(3)); // Wait 3 seconds for display
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY