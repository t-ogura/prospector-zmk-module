/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#define MAX_KEYBOARD_ENTRIES 6  // Maximum 6 keyboards displayable

// Per-keyboard display elements
struct keyboard_entry {
    lv_obj_t *container;        // Clickable container for selection
    lv_obj_t *name_label;       // Keyboard name
    lv_obj_t *rssi_bar;         // RSSI strength bar (0-5 bars)
    lv_obj_t *rssi_label;       // RSSI dBm value
    int keyboard_index;         // Index in scanner's keyboard array (for selection callback)
};

struct zmk_widget_keyboard_list {
    lv_obj_t *obj;              // Container object
    lv_obj_t *title_label;      // Title label

    struct keyboard_entry entries[MAX_KEYBOARD_ENTRIES];  // Dynamic keyboard entries
    uint8_t entry_count;        // Current number of entries displayed

    struct k_work_delayable update_work;  // Periodic update timer
    lv_obj_t *parent;           // Parent screen
};

// Dynamic allocation functions
struct zmk_widget_keyboard_list *zmk_widget_keyboard_list_create(lv_obj_t *parent);
void zmk_widget_keyboard_list_destroy(struct zmk_widget_keyboard_list *widget);

// Widget control functions
void zmk_widget_keyboard_list_show(struct zmk_widget_keyboard_list *widget);
void zmk_widget_keyboard_list_hide(struct zmk_widget_keyboard_list *widget);
void zmk_widget_keyboard_list_update(struct zmk_widget_keyboard_list *widget);
