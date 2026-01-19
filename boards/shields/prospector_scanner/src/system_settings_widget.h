/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

struct zmk_widget_system_settings {
    lv_obj_t *obj;              // Container object (NULL until first show)
    lv_obj_t *title_label;      // Title label
    lv_obj_t *bootloader_btn;   // Bootloader button
    lv_obj_t *bootloader_label; // Bootloader button label
    lv_obj_t *reset_btn;        // Reset button
    lv_obj_t *reset_label;      // Reset button label
    lv_obj_t *parent;           // Parent screen for lazy init

    // Channel selector UI elements
    lv_obj_t *channel_label;    // "Channel:" label
    lv_obj_t *channel_value;    // Current channel value display
    lv_obj_t *channel_left_btn; // Left arrow button (decrease)
    lv_obj_t *channel_right_btn; // Right arrow button (increase)
};

// Runtime channel functions (scanner can call these to get/set channel)
uint8_t scanner_get_runtime_channel(void);
void scanner_set_runtime_channel(uint8_t channel);

// Dynamic allocation functions
struct zmk_widget_system_settings *zmk_widget_system_settings_create(lv_obj_t *parent);
void zmk_widget_system_settings_destroy(struct zmk_widget_system_settings *widget);

// Widget control functions
void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget);
void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget);
