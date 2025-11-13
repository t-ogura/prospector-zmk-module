/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

struct zmk_widget_system_settings {
    lv_obj_t *obj;                  // Container object (NULL until first show)
    lv_obj_t *title_label;          // Title label
    lv_obj_t *bootloader_button;    // Bootloader button
    lv_obj_t *reset_button;         // Reset button
    lv_obj_t *instruction_label;    // Instruction text
    lv_obj_t *parent_screen;        // Parent screen for lazy initialization
};

// Lazy initialization - creates widget on first show
void zmk_widget_system_settings_init_lazy(struct zmk_widget_system_settings *widget, lv_obj_t *parent);
void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget);
void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget);
