/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

struct zmk_widget_system_settings {
    lv_obj_t *obj;           // Container object
    lv_obj_t *title_label;   // Title label only
};

int zmk_widget_system_settings_init(struct zmk_widget_system_settings *widget, lv_obj_t *parent);
void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget);
void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget);
