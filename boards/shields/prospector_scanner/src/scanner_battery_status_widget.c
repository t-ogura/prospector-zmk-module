/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/battery.h>
#include <zmk/usb.h>

#include "scanner_battery_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Color definitions for battery level indication - initialized at runtime
static lv_color_t scanner_battery_colors[6];
static bool colors_initialized = false;

// Initialize colors at runtime to avoid compile-time macro issues
static void init_battery_colors(void) {
    if (colors_initialized) return;
    
    scanner_battery_colors[SCANNER_BATTERY_ICON_FULL]     = lv_color_hex(0x00FF00); // Green
    scanner_battery_colors[SCANNER_BATTERY_ICON_HIGH]     = lv_color_hex(0x7FFF00); // Light Green
    scanner_battery_colors[SCANNER_BATTERY_ICON_MEDIUM]   = lv_color_hex(0xFFFF00); // Yellow
    scanner_battery_colors[SCANNER_BATTERY_ICON_LOW]      = lv_color_hex(0xFF7F00); // Orange
    scanner_battery_colors[SCANNER_BATTERY_ICON_CRITICAL] = lv_color_hex(0xFF0000); // Red
    scanner_battery_colors[SCANNER_BATTERY_ICON_CHARGING] = lv_color_hex(0x007FFF); // Blue
    
    colors_initialized = true;
}

// Battery icon text based on level - using LVGL built-in battery symbols
static const char* scanner_battery_icon_text[] = {
    [SCANNER_BATTERY_ICON_FULL]     = LV_SYMBOL_BATTERY_FULL,
    [SCANNER_BATTERY_ICON_HIGH]     = LV_SYMBOL_BATTERY_3, 
    [SCANNER_BATTERY_ICON_MEDIUM]   = LV_SYMBOL_BATTERY_2,
    [SCANNER_BATTERY_ICON_LOW]      = LV_SYMBOL_BATTERY_1,
    [SCANNER_BATTERY_ICON_CRITICAL] = LV_SYMBOL_BATTERY_EMPTY,
    [SCANNER_BATTERY_ICON_CHARGING] = LV_SYMBOL_CHARGE,  // Just charge symbol, battery icon added dynamically
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
    
    // Initialize colors if not done yet
    init_battery_colors();

    // Update battery icon
    if (widget->battery_icon) {
        // For charging state, show battery level icon + charge symbol
        if (icon_state == SCANNER_BATTERY_ICON_CHARGING) {
            const char* battery_symbol;
            if (battery_level >= 80) {
                battery_symbol = LV_SYMBOL_BATTERY_FULL;
            } else if (battery_level >= 60) {
                battery_symbol = LV_SYMBOL_BATTERY_3;
            } else if (battery_level >= 40) {
                battery_symbol = LV_SYMBOL_BATTERY_2;
            } else if (battery_level >= 20) {
                battery_symbol = LV_SYMBOL_BATTERY_1;
            } else {
                battery_symbol = LV_SYMBOL_BATTERY_EMPTY;
            }
            
            // Combine charge symbol on the left with battery symbol (fixed width)
            static char combined_symbol[32];
            snprintf(combined_symbol, sizeof(combined_symbol), LV_SYMBOL_CHARGE "%s", battery_symbol);
            lv_label_set_text(widget->battery_icon, combined_symbol);
        } else {
            // Normal battery state - add extra spaces to align with charging state position
            // Using 3 spaces to better match the width of the charge symbol and move 2px right
            static char padded_symbol[32];
            snprintf(padded_symbol, sizeof(padded_symbol), "  %s", scanner_battery_icon_text[icon_state]);
            lv_label_set_text(widget->battery_icon, padded_symbol);
        }
        
        lv_obj_set_style_text_color(widget->battery_icon, 
                                   scanner_battery_colors[icon_state], 0);
    }

    // Update percentage text (without % symbol)
    if (widget->percentage_label) {
        char percentage_text[8];
        snprintf(percentage_text, sizeof(percentage_text), "%d", battery_level);
        lv_label_set_text(widget->percentage_label, percentage_text);
        lv_obj_set_style_text_color(widget->percentage_label,
                                   scanner_battery_colors[icon_state], 0);
    }

    // Hide charging icon - we use battery icon to show charging state
    // This prevents duplicate display of charging status
    if (widget->charging_icon) {
        lv_obj_add_flag(widget->charging_icon, LV_OBJ_FLAG_HIDDEN);
    }

    LOG_DBG("Scanner battery widget updated: %s %d%% (state %d)",
            scanner_battery_icon_text[icon_state], battery_level, icon_state);
}

bool zmk_scanner_battery_hardware_available(void) {
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_DEMO_MODE)
    // Demo mode: always return true for UI testing
    return true;
#elif DT_HAS_CHOSEN(zmk_battery)
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
    lv_obj_set_style_text_font(widget->battery_icon, &lv_font_montserrat_12, 0);  // Smaller font
    lv_obj_align(widget->battery_icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(widget->battery_icon, "BAT");

    // Create percentage label with smaller font
    widget->percentage_label = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->percentage_label, &lv_font_unscii_8, 0);  // Smaller unscii font
    lv_obj_set_style_text_color(widget->percentage_label, lv_color_white(), 0);
    lv_obj_align_to(widget->percentage_label, widget->battery_icon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);
    lv_label_set_text(widget->percentage_label, "--");

    // Create charging icon (initially hidden)
    widget->charging_icon = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->charging_icon, &lv_font_montserrat_12, 0);
    // Initialize colors first, then set charging icon color
    init_battery_colors();
    lv_obj_set_style_text_color(widget->charging_icon, 
                               scanner_battery_colors[SCANNER_BATTERY_ICON_CHARGING], 0); // Blue
    lv_obj_align_to(widget->charging_icon, widget->percentage_label, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_label_set_text(widget->charging_icon, "CHG");
    lv_obj_add_flag(widget->charging_icon, LV_OBJ_FLAG_HIDDEN);

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

    // INVESTIGATION MODE: Log all update calls even if values haven't changed
    LOG_INF("🔍 WIDGET UPDATE CALL: Battery=%d%% (was %d%%) USB=%s (was %s) Charging=%s (was %s)",
            battery_level, widget->last_battery_level,
            usb_powered ? "true" : "false", widget->last_usb_powered ? "true" : "false",
            charging ? "true" : "false", widget->last_charging ? "true" : "false");
    
    // Check for changes to minimize updates
    if (widget->last_battery_level == battery_level && 
        widget->last_usb_powered == usb_powered &&
        widget->last_charging == charging) {
        LOG_INF("🔍 WIDGET SKIP: No changes detected - skipping visual update");
        return; // No changes
    }
    
    LOG_INF("🔍 WIDGET CHANGE DETECTED: Proceeding with visual update");

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

    // Widget visibility is now controlled by user settings in display_settings_widget
    // Do NOT override visibility here - let the user control it via the toggle
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_WIDGET_HIDE_WHEN_FULL)
    // Only auto-hide when USB powered and fully charged (100%)
    // This respects user's "visible" setting - only hides when BOTH conditions are met:
    // 1. User has visibility enabled (widget->visible is true)
    // 2. Battery is full and on USB
    if (widget->visible) {
        bool should_hide = usb_powered && !charging && (battery_level >= 100);
        if (should_hide) {
            lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
    // If user disabled visibility via settings, do nothing - keep it hidden
}

void zmk_widget_scanner_battery_status_set_visible(struct zmk_widget_scanner_battery_status *widget,
                                                  bool visible) {
    if (!widget || !widget->obj) {
        return;
    }

    if (widget->visible != visible) {
        if (visible) {
            lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        }
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
        lv_label_set_text(widget->battery_icon, "BAT");
        lv_obj_set_style_text_color(widget->battery_icon, lv_color_white(), 0);
    }

    if (widget->percentage_label) {
        lv_label_set_text(widget->percentage_label, "--");
        lv_obj_set_style_text_color(widget->percentage_label, lv_color_white(), 0);
    }

    if (widget->charging_icon) {
        lv_obj_add_flag(widget->charging_icon, LV_OBJ_FLAG_HIDDEN);
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