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

#define MAX_LAYER_DISPLAY (CONFIG_PROSPECTOR_MAX_LAYERS + 1)  // Display layers 0 to CONFIG_PROSPECTOR_MAX_LAYERS

struct zmk_widget_layer_status {
    lv_obj_t *obj;
    lv_obj_t *layer_title;                    // "Layer" title label
    lv_obj_t *layer_labels[MAX_LAYER_DISPLAY]; // Individual layer number labels (0-6)
};

int zmk_widget_layer_status_init(struct zmk_widget_layer_status *widget, lv_obj_t *parent);
void zmk_widget_layer_status_update(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd);
void zmk_widget_layer_status_reset(struct zmk_widget_layer_status *widget);
lv_obj_t *zmk_widget_layer_status_obj(struct zmk_widget_layer_status *widget);

#ifdef __cplusplus
}
#endif