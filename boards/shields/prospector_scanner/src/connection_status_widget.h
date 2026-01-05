/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zmk/status_scanner.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * LVGL 9 FIX: NO CONTAINER pattern
 * All elements created directly on parent screen to avoid freeze bug.
 */
struct zmk_widget_connection_status {
    lv_obj_t *obj;             // Points to first element for compatibility
    lv_obj_t *parent;          // Store parent for positioning
    lv_obj_t *transport_label;
    lv_obj_t *ble_profile_label;
};

// ========== Dynamic Allocation Functions ==========
struct zmk_widget_connection_status *zmk_widget_connection_status_create(lv_obj_t *parent);
void zmk_widget_connection_status_destroy(struct zmk_widget_connection_status *widget);

// ========== Widget Control Functions ==========
int zmk_widget_connection_status_init(struct zmk_widget_connection_status *widget, lv_obj_t *parent);
void zmk_widget_connection_status_update(struct zmk_widget_connection_status *widget, struct zmk_keyboard_status *kbd);
void zmk_widget_connection_status_reset(struct zmk_widget_connection_status *widget);
lv_obj_t *zmk_widget_connection_status_obj(struct zmk_widget_connection_status *widget);

#ifdef __cplusplus
}
#endif