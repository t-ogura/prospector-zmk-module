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
#include "connection_status_widget.h"
#include "layer_status_widget.h"
#include "modifier_status_widget.h"
// Profile widget removed - connection status already handled by connection_status_widget
#include "signal_status_widget.h"
#include "wpm_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

// Global UI objects for dynamic updates
static lv_obj_t *device_name_label = NULL;
static struct zmk_widget_scanner_battery battery_widget;
static struct zmk_widget_connection_status connection_widget;
static struct zmk_widget_layer_status layer_widget;
static struct zmk_widget_modifier_status modifier_widget;
// Profile widget removed - redundant with connection status widget
static struct zmk_widget_signal_status signal_widget;
static struct zmk_widget_wpm_status wpm_widget;

// Forward declaration
static void trigger_scanner_start(void);

// Scanner event callback for display updates
static void update_display_from_scanner(struct zmk_status_scanner_event_data *event_data) {
    if (!device_name_label) {
        return; // UI not ready yet
    }
    
    LOG_INF("Scanner event received: %d for keyboard %d", event_data->event, event_data->keyboard_index);
    
    int active_count = zmk_status_scanner_get_active_count();
    
    if (active_count == 0) {
        // No keyboards found
        lv_label_set_text(device_name_label, "Scanning...");
        LOG_INF("Display updated: No keyboards");
    } else {
        // Find active keyboards and display their info  
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
            if (kbd && kbd->active) {
                // Update device name (large, prominent display)
                lv_label_set_text(device_name_label, kbd->ble_name);
                
                // Update all widgets
                zmk_widget_scanner_battery_update(&battery_widget, kbd);
                zmk_widget_connection_status_update(&connection_widget, kbd);
                zmk_widget_layer_status_update(&layer_widget, kbd);
                zmk_widget_modifier_status_update(&modifier_widget, kbd);
                zmk_widget_signal_status_update(&signal_widget, kbd->rssi);
                zmk_widget_wpm_status_update(&wpm_widget, kbd);
                
                // Enhanced debug logging including modifier flags
                LOG_INF("🔧 SCANNER: Raw keyboard data - modifier_flags=0x%02X", kbd->data.modifier_flags);
                
                if (kbd->data.device_role == ZMK_DEVICE_ROLE_CENTRAL && 
                    kbd->data.peripheral_battery[0] > 0) {
                    LOG_INF("Split keyboard: %s, Central %d%%, Left %d%%, Layer: %d, Mods: 0x%02X", 
                            kbd->ble_name, kbd->data.battery_level, 
                            kbd->data.peripheral_battery[0], kbd->data.active_layer, kbd->data.modifier_flags);
                } else {
                    LOG_INF("Keyboard: %s, Battery %d%%, Layer: %d, Mods: 0x%02X", 
                            kbd->ble_name, kbd->data.battery_level, kbd->data.active_layer, kbd->data.modifier_flags);
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
    
    // Device name label at top center (larger font for better readability) - moved down 10px
    device_name_label = lv_label_create(screen);
    lv_obj_set_style_text_color(device_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_16, 0); // Retro pixel font style
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 25); // Back to center
    lv_label_set_text(device_name_label, "Initializing...");
    
    // Connection status widget in top right - moved down 30px
    zmk_widget_connection_status_init(&connection_widget, screen);
    lv_obj_align(zmk_widget_connection_status_obj(&connection_widget), LV_ALIGN_TOP_RIGHT, -5, 45); // Back to original
    
    // Layer status widget in the center (horizontal layer display) - moved down 10px
    zmk_widget_layer_status_init(&layer_widget, screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_widget), LV_ALIGN_CENTER, 0, -10); // Back to center
    
    // Modifier status widget between layer and battery - moved down 10px
    zmk_widget_modifier_status_init(&modifier_widget, screen);
    lv_obj_align(zmk_widget_modifier_status_obj(&modifier_widget), LV_ALIGN_CENTER, 0, 30); // Back to center
    
    // Profile widget removed - BLE profile already shown in connection status widget
    
    // Battery widget moved down 20px more as requested (was -40, now -20)
    zmk_widget_scanner_battery_init(&battery_widget, screen);
    lv_obj_align(zmk_widget_scanner_battery_obj(&battery_widget), LV_ALIGN_BOTTOM_MID, 0, -20); // Back to center
    lv_obj_set_height(zmk_widget_scanner_battery_obj(&battery_widget), 50);
    
    // WPM status widget - positioned above layer display, left-aligned
    zmk_widget_wpm_status_init(&wpm_widget, screen);
    lv_obj_align(zmk_widget_wpm_status_obj(&wpm_widget), LV_ALIGN_TOP_LEFT, 10, 50); // Top-left corner positioning
    
    // Signal status widget (RSSI + reception rate) at the very bottom with full width space
    zmk_widget_signal_status_init(&signal_widget, screen);
    lv_obj_align(zmk_widget_signal_status_obj(&signal_widget), LV_ALIGN_BOTTOM_RIGHT, -5, -5); // More space from edge
    
    // Trigger scanner initialization after screen is ready
    trigger_scanner_start();
    
    LOG_INF("Scanner screen created successfully");
    return screen;
}

// Late initialization to start scanner after display is ready
static void start_scanner_delayed(struct k_work *work) {
    if (!device_name_label) {
        LOG_WRN("Display not ready yet, retrying scanner start...");
        k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(1));
        return;
    }
    
    LOG_INF("Starting BLE scanner...");
    lv_label_set_text(device_name_label, "Starting scanner...");
    
    // Register callback first
    int ret = zmk_status_scanner_register_callback(update_display_from_scanner);
    if (ret < 0) {
        LOG_ERR("Failed to register scanner callback: %d", ret);
        lv_label_set_text(device_name_label, "Scanner Error");
        return;
    }
    
    // Start scanning
    ret = zmk_status_scanner_start();
    if (ret < 0) {
        LOG_ERR("Failed to start scanner: %d", ret);
        lv_label_set_text(device_name_label, "Start Error");
        return;
    }
    
    LOG_INF("BLE scanner started successfully");
    lv_label_set_text(device_name_label, "Scanning...");
}

static K_WORK_DELAYABLE_DEFINE(scanner_start_work, start_scanner_delayed);

// Trigger scanner start automatically when screen is created
static void trigger_scanner_start(void) {
    LOG_INF("Scheduling delayed scanner start from display creation");
    k_work_schedule(&scanner_start_work, K_SECONDS(3)); // Wait 3 seconds for display
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY