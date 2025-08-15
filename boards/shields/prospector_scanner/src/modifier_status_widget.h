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

struct zmk_widget_modifier_status {
    lv_obj_t *obj;
    lv_obj_t *label; // Single label for YADS-style display
};

int zmk_widget_modifier_status_init(struct zmk_widget_modifier_status *widget, lv_obj_t *parent);
void zmk_widget_modifier_status_update(struct zmk_widget_modifier_status *widget, struct zmk_keyboard_status *kbd);
void zmk_widget_modifier_status_reset(struct zmk_widget_modifier_status *widget);
lv_obj_t *zmk_widget_modifier_status_obj(struct zmk_widget_modifier_status *widget);

#ifdef __cplusplus
}
#endif