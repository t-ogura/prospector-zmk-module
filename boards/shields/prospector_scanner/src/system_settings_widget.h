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
};

int zmk_widget_system_settings_init(struct zmk_widget_system_settings *widget, lv_obj_t *parent);
void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget);
void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget);
