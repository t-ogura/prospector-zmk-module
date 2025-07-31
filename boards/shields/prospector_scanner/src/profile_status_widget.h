/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zephyr/sys/util.h>

struct zmk_widget_profile_status {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_obj_t *profile_label;
};

int zmk_widget_profile_status_init(struct zmk_widget_profile_status *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_profile_status_obj(struct zmk_widget_profile_status *widget);