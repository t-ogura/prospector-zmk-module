/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <zmk/display.h>
#include <zmk/status_scanner.h>

#include <lvgl.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

static lv_obj_t *screen;
static lv_obj_t *keyboard_name_label;
static lv_obj_t *layer_label;
static lv_obj_t *battery_bar;
static lv_obj_t *battery_label;
static lv_obj_t *connection_label;
static lv_obj_t *status_label;

static void update_display(void) {
    int primary = zmk_status_scanner_get_primary_keyboard();
    
    if (primary >= 0) {
        struct zmk_keyboard_status *status = zmk_status_scanner_get_keyboard(primary);
        if (status) {
            // Update keyboard name
            char name_buf[32];
            snprintf(name_buf, sizeof(name_buf), "%.8s", status->data.layer_name);
            lv_label_set_text(keyboard_name_label, name_buf);
            
            // Update layer info
            char layer_buf[32];
            snprintf(layer_buf, sizeof(layer_buf), "Layer: %s", status->data.layer_name);
            lv_label_set_text(layer_label, layer_buf);
            
            // Update battery
            lv_bar_set_value(battery_bar, status->data.battery_level, LV_ANIM_OFF);
            char battery_buf[16];
            snprintf(battery_buf, sizeof(battery_buf), "%d%%", status->data.battery_level);
            lv_label_set_text(battery_label, battery_buf);
            
            // Update connection info
            char conn_buf[32];
            snprintf(conn_buf, sizeof(conn_buf), "Devices: %d/5", status->data.connection_count);
            lv_label_set_text(connection_label, conn_buf);
            
            // Update status flags
            char status_buf[64] = "";
            if (status->data.status_flags & ZMK_STATUS_FLAG_CAPS_WORD) {
                strcat(status_buf, "CAPS ");
            }
            if (status->data.status_flags & ZMK_STATUS_FLAG_CHARGING) {
                strcat(status_buf, "CHG ");
            }
            if (status->data.status_flags & ZMK_STATUS_FLAG_USB_CONNECTED) {
                strcat(status_buf, "USB ");
            }
            if (strlen(status_buf) == 0) {
                strcpy(status_buf, "Ready");
            }
            lv_label_set_text(status_label, status_buf);
        }
    } else {
        // No keyboards found
        lv_label_set_text(keyboard_name_label, "Scanning...");
        lv_label_set_text(layer_label, "No keyboards found");
        lv_bar_set_value(battery_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(battery_label, "");
        lv_label_set_text(connection_label, "");
        lv_label_set_text(status_label, "Waiting for keyboards");
    }
}

static void scanner_event_callback(struct zmk_status_scanner_event_data *event_data) {
    LOG_INF("Scanner event: %d for keyboard %d", event_data->event, event_data->keyboard_index);
    update_display();
}

static int create_display_ui(void) {
    screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    
    // Keyboard name (top)
    keyboard_name_label = lv_label_create(screen);
    lv_obj_set_style_text_color(keyboard_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(keyboard_name_label, &lv_font_montserrat_20, 0);
    lv_obj_align(keyboard_name_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(keyboard_name_label, "Prospector Scanner");
    
    // Layer info
    layer_label = lv_label_create(screen);
    lv_obj_set_style_text_color(layer_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_16, 0);
    lv_obj_align(layer_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(layer_label, "Initializing...");
    
    // Battery bar
    battery_bar = lv_bar_create(screen);
    lv_obj_set_size(battery_bar, 120, 20);
    lv_obj_align(battery_bar, LV_ALIGN_CENTER, 0, -20);
    lv_bar_set_range(battery_bar, 0, 100);
    lv_obj_set_style_bg_color(battery_bar, lv_color_make(64, 64, 64), 0);
    lv_obj_set_style_bg_color(battery_bar, lv_color_make(0, 200, 0), LV_PART_INDICATOR);
    
    // Battery percentage
    battery_label = lv_label_create(screen);
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_14, 0);
    lv_obj_align(battery_label, LV_ALIGN_CENTER, 0, 10);
    lv_label_set_text(battery_label, "");
    
    // Connection info
    connection_label = lv_label_create(screen);
    lv_obj_set_style_text_color(connection_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(connection_label, &lv_font_montserrat_14, 0);
    lv_obj_align(connection_label, LV_ALIGN_CENTER, 0, 35);
    lv_label_set_text(connection_label, "");
    
    // Status flags
    status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_label, lv_color_make(255, 255, 0), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(status_label, "Starting...");
    
    return 0;
}

static int scanner_display_init(void) {
    LOG_INF("Initializing scanner display");
    
    // Create UI
    int ret = create_display_ui();
    if (ret < 0) {
        LOG_ERR("Failed to create display UI: %d", ret);
        return ret;
    }
    
    // Register scanner callback
    ret = zmk_status_scanner_register_callback(scanner_event_callback);
    if (ret < 0) {
        LOG_ERR("Failed to register scanner callback: %d", ret);
        return ret;
    }
    
    // Start scanning
    ret = zmk_status_scanner_start();
    if (ret < 0) {
        LOG_ERR("Failed to start scanner: %d", ret);
        return ret;
    }
    
    LOG_INF("Scanner display initialized");
    return 0;
}

SYS_INIT(scanner_display_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY