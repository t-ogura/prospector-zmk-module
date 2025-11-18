/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>

struct zmk_widget_keyboard_list {
    lv_obj_t *obj;              // Container object
    lv_obj_t *title_label;      // Title label
    lv_obj_t *kb1_label;        // First keyboard
    lv_obj_t *kb2_label;        // Second keyboard
    lv_obj_t *kb3_label;        // Third keyboard
    lv_obj_t *parent;           // Parent screen
};

// Dynamic allocation functions
struct zmk_widget_keyboard_list *zmk_widget_keyboard_list_create(lv_obj_t *parent);
void zmk_widget_keyboard_list_destroy(struct zmk_widget_keyboard_list *widget);

// Widget control functions
void zmk_widget_keyboard_list_show(struct zmk_widget_keyboard_list *widget);
void zmk_widget_keyboard_list_hide(struct zmk_widget_keyboard_list *widget);
void zmk_widget_keyboard_list_update(struct zmk_widget_keyboard_list *widget);
