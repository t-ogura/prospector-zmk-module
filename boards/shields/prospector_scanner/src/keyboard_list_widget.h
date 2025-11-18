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

    // Keyboard 1 elements
    lv_obj_t *kb1_label;        // Keyboard name
    lv_obj_t *kb1_rssi_bar;     // RSSI strength bar (0-5 bars)
    lv_obj_t *kb1_rssi_label;   // RSSI dBm value

    // Keyboard 2 elements
    lv_obj_t *kb2_label;        // Keyboard name
    lv_obj_t *kb2_rssi_bar;     // RSSI strength bar (0-5 bars)
    lv_obj_t *kb2_rssi_label;   // RSSI dBm value

    // Keyboard 3 elements
    lv_obj_t *kb3_label;        // Keyboard name
    lv_obj_t *kb3_rssi_bar;     // RSSI strength bar (0-5 bars)
    lv_obj_t *kb3_rssi_label;   // RSSI dBm value

    lv_obj_t *parent;           // Parent screen
};

// Dynamic allocation functions
struct zmk_widget_keyboard_list *zmk_widget_keyboard_list_create(lv_obj_t *parent);
void zmk_widget_keyboard_list_destroy(struct zmk_widget_keyboard_list *widget);

// Widget control functions
void zmk_widget_keyboard_list_show(struct zmk_widget_keyboard_list *widget);
void zmk_widget_keyboard_list_hide(struct zmk_widget_keyboard_list *widget);
void zmk_widget_keyboard_list_update(struct zmk_widget_keyboard_list *widget);
