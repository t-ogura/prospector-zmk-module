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

struct zmk_widget_connection_status {
    lv_obj_t *obj;
    lv_obj_t *transport_label;
    lv_obj_t *ble_profile_label;
};

int zmk_widget_connection_status_init(struct zmk_widget_connection_status *widget, lv_obj_t *parent);
void zmk_widget_connection_status_update(struct zmk_widget_connection_status *widget, struct zmk_keyboard_status *kbd);
void zmk_widget_connection_status_reset(struct zmk_widget_connection_status *widget);
lv_obj_t *zmk_widget_connection_status_obj(struct zmk_widget_connection_status *widget);

#ifdef __cplusplus
}
#endif