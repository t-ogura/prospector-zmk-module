/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zmk_widget_debug_status {
    lv_obj_t *obj;
    lv_obj_t *debug_label;
};

int zmk_widget_debug_status_init(struct zmk_widget_debug_status *widget, lv_obj_t *parent);
void zmk_widget_debug_status_set_text(struct zmk_widget_debug_status *widget, const char *text);
void zmk_widget_debug_status_set_visible(struct zmk_widget_debug_status *widget, bool visible);
lv_obj_t *zmk_widget_debug_status_obj(struct zmk_widget_debug_status *widget);

#ifdef __cplusplus
}
#endif