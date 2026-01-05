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
    // Show current active transport with > marker
    // USB is active when USB_HID_READY flag is set
    // BLE is active when connected but USB is not ready
    if (usb_hid_ready) {
        // USB is active transport
        snprintf(transport_text, sizeof(transport_text),
                "> #%s USB#\\n#%s BLE#", usb_color, ble_color);
    } else {
        // BLE is active transport (or no transport)
        snprintf(transport_text, sizeof(transport_text),
                "#%s USB#\\n> #%s BLE#", usb_color, ble_color);
    }
    
    // Update transport label with colors
    lv_label_set_recolor(widget->transport_label, true);
    lv_obj_set_style_text_align(widget->transport_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(widget->transport_label, transport_text);
    
    // Update BLE profile number (0-indexed for display)
    char profile_text[8];
    snprintf(profile_text, sizeof(profile_text), "%d", kbd->data.profile_slot);
    lv_label_set_text(widget->ble_profile_label, profile_text);
    
    LOG_INF("Connection status: USB:%s BLE:%s Profile:%d (status_flags=0x%02X)",
            usb_hid_ready ? "Ready" : "NotReady",
            ble_connected ? "Connected" : (ble_bonded ? "Bonded" : "Open"),
            kbd->data.profile_slot,
            kbd->data.status_flags);
}

/**
 * LVGL 9 FIX: NO CONTAINER - Create all elements directly on parent
 * Widget positioned at TOP_RIGHT with x=-5, y=45 in scanner_display.c
 */
int zmk_widget_connection_status_init(struct zmk_widget_connection_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }

    widget->parent = parent;

    // Position offsets from TOP_RIGHT
    const int X_OFFSET = -5;
    const int Y_OFFSET = 45;

    // Transport status label (USB/BLE with colors) - created directly on parent
    widget->transport_label = lv_label_create(parent);
    lv_obj_align(widget->transport_label, LV_ALIGN_TOP_RIGHT, X_OFFSET, Y_OFFSET);
    lv_obj_set_style_text_font(widget->transport_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(widget->transport_label, LV_TEXT_ALIGN_RIGHT, 0);

    // BLE profile number label - created directly on parent
    widget->ble_profile_label = lv_label_create(parent);
    lv_obj_align(widget->ble_profile_label, LV_ALIGN_TOP_RIGHT, X_OFFSET - 3, Y_OFFSET + 33);
    lv_obj_set_style_text_font(widget->ble_profile_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widget->ble_profile_label, lv_color_white(), 0);

    // Initialize with default values
    lv_label_set_text(widget->transport_label, "#ff0000 USB#\\n#ffffff BLE#");
    lv_label_set_text(widget->ble_profile_label, "0");
    lv_label_set_recolor(widget->transport_label, true);

    // Set widget->obj to first element for compatibility
    widget->obj = widget->transport_label;

    LOG_INF("✨ Connection status widget initialized (LVGL9 no-container pattern)");
    return 0;
}

void zmk_widget_connection_status_update(struct zmk_widget_connection_status *widget, struct zmk_keyboard_status *kbd) {
    update_connection_status(widget, kbd);
}

void zmk_widget_connection_status_reset(struct zmk_widget_connection_status *widget) {
    if (!widget || !widget->transport_label || !widget->ble_profile_label) {
        return;
    }
    
    LOG_INF("Connection widget reset - clearing connection status");
    
    // Reset to default disconnected state
    lv_label_set_text(widget->transport_label, "#ff0000 USB#\\n#ffffff BLE#");
    lv_label_set_text(widget->ble_profile_label, "-");
}

lv_obj_t *zmk_widget_connection_status_obj(struct zmk_widget_connection_status *widget) {
    return widget ? widget->obj : NULL;
}

// ========== Dynamic Allocation Functions ==========

struct zmk_widget_connection_status *zmk_widget_connection_status_create(lv_obj_t *parent) {
    LOG_DBG("Creating connection status widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory for widget structure using LVGL's memory allocator
    struct zmk_widget_connection_status *widget =
        (struct zmk_widget_connection_status *)lv_malloc(sizeof(struct zmk_widget_connection_status));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for connection_status_widget (%d bytes)",
                sizeof(struct zmk_widget_connection_status));
        return NULL;
    }

    // Zero-initialize the allocated memory
    memset(widget, 0, sizeof(struct zmk_widget_connection_status));

    // Initialize widget (this creates LVGL objects)
    int ret = zmk_widget_connection_status_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_free(widget);
        return NULL;
    }

    LOG_DBG("Connection status widget created successfully");
    return widget;
}

void zmk_widget_connection_status_destroy(struct zmk_widget_connection_status *widget) {
    LOG_DBG("Destroying connection status widget (LVGL9 no-container)");

    if (!widget) {
        return;
    }

    // LVGL 9 FIX: Delete each element individually (no container parent)
    if (widget->ble_profile_label) {
        lv_obj_del(widget->ble_profile_label);
        widget->ble_profile_label = NULL;
    }
    if (widget->transport_label) {
        lv_obj_del(widget->transport_label);
        widget->transport_label = NULL;
    }

    widget->obj = NULL;
    widget->parent = NULL;

    // Free the widget structure memory from LVGL heap
    lv_free(widget);
}