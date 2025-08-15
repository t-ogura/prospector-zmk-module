/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scanner Battery Status Widget
 * 
 * Displays Prospector scanner's own battery level and charging status
 * in the top-right corner of the display.
 * 
 * Features:
 * - Battery percentage display
 * - Color-coded battery level (green/yellow/orange/red)
 * - Charging indicator when USB connected
 * - Auto-hide when no battery hardware detected
 * - Configurable position and appearance
 */

struct zmk_widget_scanner_battery_status {
    sys_snode_t node;
    lv_obj_t *obj;                // Container object
    lv_obj_t *battery_icon;       // Battery icon (🔋/🪫) 
    lv_obj_t *percentage_label;   // Percentage text
    lv_obj_t *charging_icon;      // Charging indicator (⚡)
    
    // State cache for optimization
    uint8_t last_battery_level;   // 0-100%
    bool last_usb_powered;        // USB connection state
    bool last_charging;           // Charging state
    bool visible;                 // Widget visibility
    uint32_t last_update;         // Last update timestamp
};

/**
 * Battery icon state for visual representation
 */
typedef enum {
    SCANNER_BATTERY_ICON_FULL,      // 🔋 Green (80-100%)
    SCANNER_BATTERY_ICON_HIGH,      // 🔋 Light Green (60-79%)
    SCANNER_BATTERY_ICON_MEDIUM,    // 🔋 Yellow (40-59%)
    SCANNER_BATTERY_ICON_LOW,       // 🔋 Orange (20-39%)
    SCANNER_BATTERY_ICON_CRITICAL,  // 🪫 Red (0-19%)
    SCANNER_BATTERY_ICON_CHARGING,  // ⚡ Blue (USB connected)
    SCANNER_BATTERY_ICON_HIDDEN     // No display
} scanner_battery_icon_state_t;

/**
 * Initialize the scanner battery status widget
 * 
 * @param widget Widget structure to initialize
 * @param parent Parent LVGL object (screen)
 * @return 0 on success, negative error code on failure
 */
int zmk_widget_scanner_battery_status_init(struct zmk_widget_scanner_battery_status *widget, 
                                          lv_obj_t *parent);

/**
 * Get the widget's LVGL object for positioning
 * 
 * @param widget Widget structure
 * @return LVGL object or NULL if not initialized
 */
lv_obj_t *zmk_widget_scanner_battery_status_obj(struct zmk_widget_scanner_battery_status *widget);

/**
 * Update battery status display
 * 
 * @param widget Widget to update
 * @param battery_level Battery level (0-100%)
 * @param usb_powered True if USB connected
 * @param charging True if currently charging
 */
void zmk_widget_scanner_battery_status_update(struct zmk_widget_scanner_battery_status *widget,
                                             uint8_t battery_level,
                                             bool usb_powered, 
                                             bool charging);

/**
 * Set widget visibility
 * 
 * @param widget Widget to modify
 * @param visible True to show, false to hide
 */
void zmk_widget_scanner_battery_status_set_visible(struct zmk_widget_scanner_battery_status *widget,
                                                  bool visible);

/**
 * Reset widget to default state
 * 
 * @param widget Widget to reset
 */
void zmk_widget_scanner_battery_status_reset(struct zmk_widget_scanner_battery_status *widget);

/**
 * Check if battery hardware is available
 * 
 * @return True if battery hardware detected, false otherwise
 */
bool zmk_scanner_battery_hardware_available(void);

#ifdef __cplusplus
}
#endif