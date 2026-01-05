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
struct zmk_widget_modifier_status {
    lv_obj_t *obj;     // Points to first element for compatibility
    lv_obj_t *parent;  // Store parent for positioning
    lv_obj_t *label;   // Single label for YADS-style display
};

// Dynamic allocation functions
struct zmk_widget_modifier_status *zmk_widget_modifier_status_create(lv_obj_t *parent);
void zmk_widget_modifier_status_destroy(struct zmk_widget_modifier_status *widget);

// Widget control functions
void zmk_widget_modifier_status_update(struct zmk_widget_modifier_status *widget, struct zmk_keyboard_status *kbd);
void zmk_widget_modifier_status_reset(struct zmk_widget_modifier_status *widget);
lv_obj_t *zmk_widget_modifier_status_obj(struct zmk_widget_modifier_status *widget);

#ifdef __cplusplus
}
#endif