/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

struct zmk_widget_display_settings {
    lv_obj_t *obj;                  // Container object
    lv_obj_t *title_label;          // Title label

    // Brightness section
    lv_obj_t *brightness_label;     // "Brightness" label
    lv_obj_t *auto_brightness_sw;   // Auto brightness toggle (only if sensor enabled)
    lv_obj_t *brightness_slider;    // Manual brightness slider
    lv_obj_t *brightness_value;     // Current brightness value label

    // Battery widget section
    lv_obj_t *battery_label;        // "Battery Widget" label
    lv_obj_t *battery_sw;           // Battery widget toggle

    // Layer count section
    lv_obj_t *layer_label;          // "Max Layers" label
    lv_obj_t *layer_slider;         // Layer count slider (4-10)
    lv_obj_t *layer_value;          // Current layer count label

    lv_obj_t *parent;               // Parent screen for lazy init

    // State
    bool auto_brightness_enabled;   // Current auto brightness state
    uint8_t manual_brightness;      // Current manual brightness (0-100)
    bool battery_widget_visible;    // Battery widget visibility
    uint8_t max_layers;             // Max layer count (4-10)
};

// Dynamic allocation functions
struct zmk_widget_display_settings *zmk_widget_display_settings_create(lv_obj_t *parent);
void zmk_widget_display_settings_destroy(struct zmk_widget_display_settings *widget);

// Widget control functions
void zmk_widget_display_settings_show(struct zmk_widget_display_settings *widget);
void zmk_widget_display_settings_hide(struct zmk_widget_display_settings *widget);

// Get current settings (for applying to system)
uint8_t zmk_widget_display_settings_get_brightness(struct zmk_widget_display_settings *widget);
bool zmk_widget_display_settings_get_auto_brightness(struct zmk_widget_display_settings *widget);
bool zmk_widget_display_settings_get_battery_visible(struct zmk_widget_display_settings *widget);
uint8_t zmk_widget_display_settings_get_max_layers(struct zmk_widget_display_settings *widget);

// Check if user is interacting with UI (prevents swipe gestures)
bool display_settings_is_interacting(void);

// Global settings accessors (persist even when widget is destroyed)
bool display_settings_get_battery_visible_global(void);
uint8_t display_settings_get_max_layers_global(void);
