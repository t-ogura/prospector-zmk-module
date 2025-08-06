/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/battery.h>
#include <zmk/usb.h>

#include "scanner_battery_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Color definitions for battery level indication
static const lv_color_t scanner_battery_colors[] = {
    [SCANNER_BATTERY_ICON_FULL]     = LV_COLOR_MAKE(0x00, 0xFF, 0x00), // Green
    [SCANNER_BATTERY_ICON_HIGH]     = LV_COLOR_MAKE(0x7F, 0xFF, 0x00), // Light Green
    [SCANNER_BATTERY_ICON_MEDIUM]   = LV_COLOR_MAKE(0xFF, 0xFF, 0x00), // Yellow
    [SCANNER_BATTERY_ICON_LOW]      = LV_COLOR_MAKE(0xFF, 0x7F, 0x00), // Orange
    [SCANNER_BATTERY_ICON_CRITICAL] = LV_COLOR_MAKE(0xFF, 0x00, 0x00), // Red
    [SCANNER_BATTERY_ICON_CHARGING] = LV_COLOR_MAKE(0x00, 0x7F, 0xFF), // Blue
};

// Battery icon text based on level
static const char* scanner_battery_icon_text[] = {
    [SCANNER_BATTERY_ICON_FULL]     = "🔋",
    [SCANNER_BATTERY_ICON_HIGH]     = "🔋", 
    [SCANNER_BATTERY_ICON_MEDIUM]   = "🔋",
    [SCANNER_BATTERY_ICON_LOW]      = "🔋",
    [SCANNER_BATTERY_ICON_CRITICAL] = "🪫",
    [SCANNER_BATTERY_ICON_CHARGING] = "⚡",
    [SCANNER_BATTERY_ICON_HIDDEN]   = ""
};

/**
 * Determine battery icon state based on level and USB status
 */
static scanner_battery_icon_state_t get_battery_icon_state(uint8_t battery_level, 
                                                          bool usb_powered, 
                                                          bool charging) {
    // If USB connected and charging, show charging icon
    if (usb_powered && charging) {
        return SCANNER_BATTERY_ICON_CHARGING;
    }
    
    // Battery level based icons
    if (battery_level >= 80) {
        return SCANNER_BATTERY_ICON_FULL;
    } else if (battery_level >= 60) {
        return SCANNER_BATTERY_ICON_HIGH;
    } else if (battery_level >= 40) {
        return SCANNER_BATTERY_ICON_MEDIUM;
    } else if (battery_level >= 20) {
        return SCANNER_BATTERY_ICON_LOW;
    } else {
        return SCANNER_BATTERY_ICON_CRITICAL;
    }
}

/**
 * Update widget visual appearance based on state
 */
static void update_widget_appearance(struct zmk_widget_scanner_battery_status *widget,
                                   scanner_battery_icon_state_t icon_state,
                                   uint8_t battery_level) {
    if (!widget || !widget->obj) {
        return;
    }

    // Update battery icon
    if (widget->battery_icon) {
        lv_label_set_text(widget->battery_icon, scanner_battery_icon_text[icon_state]);
        lv_obj_set_style_text_color(widget->battery_icon, 
                                   scanner_battery_colors[icon_state], 0);
    }

    // Update percentage text
    if (widget->percentage_label) {
        char percentage_text[8];
        snprintf(percentage_text, sizeof(percentage_text), "%d%%", battery_level);
        lv_label_set_text(widget->percentage_label, percentage_text);
        lv_obj_set_style_text_color(widget->percentage_label,
                                   scanner_battery_colors[icon_state], 0);
    }

    // Show/hide charging icon
    if (widget->charging_icon) {
        bool show_charging = (icon_state == SCANNER_BATTERY_ICON_CHARGING);
        lv_obj_set_hidden(widget->charging_icon, !show_charging);
    }

    LOG_DBG("Scanner battery widget updated: %s %d%% (state %d)",
            scanner_battery_icon_text[icon_state], battery_level, icon_state);
}

bool zmk_scanner_battery_hardware_available(void) {
#if DT_HAS_CHOSEN(zmk_battery)
    const struct device *battery_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
    return device_is_ready(battery_dev);
#else
    return false;
#endif
}

int zmk_widget_scanner_battery_status_init(struct zmk_widget_scanner_battery_status *widget, 
                                          lv_obj_t *parent) {
    if (!widget || !parent) {
        return -EINVAL;
    }

    // Check if battery hardware is available
    if (!zmk_scanner_battery_hardware_available()) {
        LOG_INF("Scanner battery hardware not detected - widget will be hidden");
        widget->visible = false;
        return 0;
    }

    LOG_INF("Initializing scanner battery status widget");

    // Create container object
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 80, 25); // Compact size for top-right corner
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0); // Transparent background
    lv_obj_set_style_border_width(widget->obj, 0, 0);
    lv_obj_set_style_pad_all(widget->obj, 2, 0);

    // Create battery icon label
    widget->battery_icon = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->battery_icon, &lv_font_montserrat_16, 0);
    lv_obj_align(widget->battery_icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(widget->battery_icon, "🔋");

    // Create percentage label  
    widget->percentage_label = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->percentage_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widget->percentage_label, lv_color_white(), 0);
    lv_obj_align_to(widget->percentage_label, widget->battery_icon, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_label_set_text(widget->percentage_label, "--");

    // Create charging icon (initially hidden)
    widget->charging_icon = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->charging_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widget->charging_icon, 
                               LV_COLOR_MAKE(0x00, 0x7F, 0xFF), 0); // Blue
    lv_obj_align_to(widget->charging_icon, widget->percentage_label, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_label_set_text(widget->charging_icon, "⚡");
    lv_obj_set_hidden(widget->charging_icon, true);

    // Initialize state
    widget->last_battery_level = 0;
    widget->last_usb_powered = false;
    widget->last_charging = false;
    widget->visible = true;
    widget->last_update = 0;

    // Get initial battery status and update display
    uint8_t initial_level = 0;
    bool initial_usb = false;
    
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    initial_level = zmk_battery_state_of_charge();
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)  
    initial_usb = zmk_usb_is_powered();
#endif

    zmk_widget_scanner_battery_status_update(widget, initial_level, initial_usb, initial_usb);

    LOG_INF("Scanner battery status widget initialized successfully (level: %d%%, USB: %s)",
            initial_level, initial_usb ? "yes" : "no");

    return 0;
}

lv_obj_t *zmk_widget_scanner_battery_status_obj(struct zmk_widget_scanner_battery_status *widget) {
    return widget ? widget->obj : NULL;
}

void zmk_widget_scanner_battery_status_update(struct zmk_widget_scanner_battery_status *widget,
                                             uint8_t battery_level,
                                             bool usb_powered,
                                             bool charging) {
    if (!widget) {
        return;
    }

    // Check if hardware is available
    if (!zmk_scanner_battery_hardware_available()) {
        zmk_widget_scanner_battery_status_set_visible(widget, false);
        return;
    }

    // Check for changes to minimize updates
    if (widget->last_battery_level == battery_level && 
        widget->last_usb_powered == usb_powered &&
        widget->last_charging == charging) {
        return; // No changes
    }

    LOG_DBG("Scanner battery status update: %d%% USB=%s charging=%s",
            battery_level, usb_powered ? "yes" : "no", charging ? "yes" : "no");

    // Determine icon state and update appearance
    scanner_battery_icon_state_t icon_state = get_battery_icon_state(battery_level, usb_powered, charging);
    update_widget_appearance(widget, icon_state, battery_level);

    // Cache current state
    widget->last_battery_level = battery_level;
    widget->last_usb_powered = usb_powered;  
    widget->last_charging = charging;
    widget->last_update = k_uptime_get_32();

    // Widget visibility based on configuration
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_WIDGET_HIDE_WHEN_FULL)
    // Hide when USB powered and fully charged (100%)
    bool should_hide = usb_powered && !charging && (battery_level >= 100);
    zmk_widget_scanner_battery_status_set_visible(widget, !should_hide);
#else
    // Always show when hardware is available
    zmk_widget_scanner_battery_status_set_visible(widget, true);
#endif
}

void zmk_widget_scanner_battery_status_set_visible(struct zmk_widget_scanner_battery_status *widget,
                                                  bool visible) {
    if (!widget || !widget->obj) {
        return;
    }

    if (widget->visible != visible) {
        lv_obj_set_hidden(widget->obj, !visible);
        widget->visible = visible;
        LOG_DBG("Scanner battery widget visibility: %s", visible ? "shown" : "hidden");
    }
}

void zmk_widget_scanner_battery_status_reset(struct zmk_widget_scanner_battery_status *widget) {
    if (!widget) {
        return;
    }

    LOG_DBG("Resetting scanner battery status widget");

    // Reset to default state
    if (widget->battery_icon) {
        lv_label_set_text(widget->battery_icon, "🔋");
        lv_obj_set_style_text_color(widget->battery_icon, lv_color_white(), 0);
    }

    if (widget->percentage_label) {
        lv_label_set_text(widget->percentage_label, "--");
        lv_obj_set_style_text_color(widget->percentage_label, lv_color_white(), 0);
    }

    if (widget->charging_icon) {
        lv_obj_set_hidden(widget->charging_icon, true);
    }

    // Reset cached state
    widget->last_battery_level = 0;
    widget->last_usb_powered = false;
    widget->last_charging = false;
    widget->last_update = 0;

    // Hide if no battery hardware
    if (!zmk_scanner_battery_hardware_available()) {
        zmk_widget_scanner_battery_status_set_visible(widget, false);
    }
}