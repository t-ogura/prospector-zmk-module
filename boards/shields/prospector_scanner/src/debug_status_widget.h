/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>

struct zmk_widget_debug_status {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *debug_label;
};

int zmk_widget_debug_status_init(struct zmk_widget_debug_status *widget, lv_obj_t *parent);
int zmk_widget_debug_status_deinit(struct zmk_widget_debug_status *widget);
void zmk_widget_debug_status_set_text(struct zmk_widget_debug_status *widget, const char *text);
void zmk_widget_debug_status_set_visible(struct zmk_widget_debug_status *widget, bool visible);