/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/logging/log.h>
#include <zmk/status_advertisement.h>
#include <zmk/status_scanner.h>
#include "connection_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static void update_connection_status(struct zmk_widget_connection_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !kbd) {
        return;
    }
    
    // Get USB and BLE status from status_flags
    bool usb_hid_ready = kbd->data.status_flags & ZMK_STATUS_FLAG_USB_HID_READY;
    bool ble_connected = kbd->data.status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED;
    bool ble_bonded = kbd->data.status_flags & ZMK_STATUS_FLAG_BLE_BONDED;
    
    // Determine USB and BLE colors based on YADS implementation
    const char *usb_color = "#ff0000"; // Default red (not ready)
    const char *ble_color = "#ffffff"; // Default white (not connected/bonded)
    
    if (usb_hid_ready) {
        usb_color = "#ffffff"; // White when ready
    }
    
    if (ble_connected) {
        ble_color = "#00ff00"; // Green when connected
    } else if (ble_bonded) {
        ble_color = "#0000ff"; // Blue when bonded but not connected
    }
    
    // Format transport text similar to YADS
    char transport_text[64];
    // Show current active profile with > marker
    if (kbd->data.profile_slot >= 0 && kbd->data.profile_slot <= 4) {
        // Assume BLE is active if profile_slot is valid
        snprintf(transport_text, sizeof(transport_text), 
                "#%s USB#\\n> #%s BLE#", usb_color, ble_color);
    } else {
        // USB might be active
        snprintf(transport_text, sizeof(transport_text), 
                "> #%s USB#\\n#%s BLE#", usb_color, ble_color);
    }
    
    // Update transport label with colors
    lv_label_set_recolor(widget->transport_label, true);
    lv_obj_set_style_text_align(widget->transport_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(widget->transport_label, transport_text);
    
    // Update BLE profile number (0-indexed for display)
    char profile_text[8];
    snprintf(profile_text, sizeof(profile_text), "%d", kbd->data.profile_slot);
    lv_label_set_text(widget->ble_profile_label, profile_text);
    
    LOG_INF("Connection status: USB:%s BLE:%s Profile:%d", 
            usb_hid_ready ? "Ready" : "NotReady",
            ble_connected ? "Connected" : (ble_bonded ? "Bonded" : "Open"),
            kbd->data.profile_slot);
}

int zmk_widget_connection_status_init(struct zmk_widget_connection_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }
    
    // Create container widget sized for connection status display
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 80, 60); // Compact size for circular display
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    
    // Transport status label (USB/BLE with colors)
    widget->transport_label = lv_label_create(widget->obj);
    lv_obj_align(widget->transport_label, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_obj_set_style_text_font(widget->transport_label, &lv_font_montserrat_12, 0);
    
    // BLE profile number label (positioned closer to BLE text)
    widget->ble_profile_label = lv_label_create(widget->obj);
    lv_obj_align(widget->ble_profile_label, LV_ALIGN_BOTTOM_RIGHT, -2, -8); // Closer spacing
    lv_obj_set_style_text_font(widget->ble_profile_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widget->ble_profile_label, lv_color_white(), 0);
    
    // Initialize with default values
    lv_label_set_text(widget->transport_label, "#ff0000 USB#\\n#ffffff BLE#");
    lv_label_set_text(widget->ble_profile_label, "0");
    lv_label_set_recolor(widget->transport_label, true);
    
    LOG_INF("Connection status widget initialized");
    return 0;
}

void zmk_widget_connection_status_update(struct zmk_widget_connection_status *widget, struct zmk_keyboard_status *kbd) {
    update_connection_status(widget, kbd);
}

lv_obj_t *zmk_widget_connection_status_obj(struct zmk_widget_connection_status *widget) {
    return widget ? widget->obj : NULL;
}