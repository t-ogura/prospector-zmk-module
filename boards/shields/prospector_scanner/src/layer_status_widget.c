/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/logging/log.h>
#include <zmk/status_advertisement.h>
#include <zmk/status_scanner.h>
#include "layer_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static void update_layer_display(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !kbd) {
        return;
    }
    
    uint8_t active_layer = kbd->data.active_layer;
    
    // Update each layer label (0-6)
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        if (i == active_layer) {
            // Active layer: bright white with larger font
            lv_obj_set_style_text_color(widget->layer_labels[i], lv_color_white(), 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_COVER, 0);
        } else {
            // Inactive layers: dimmed gray
            lv_obj_set_style_text_color(widget->layer_labels[i], lv_color_make(80, 80, 80), 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_50, 0);
        }
    }
    
    LOG_DBG("Layer display updated: active layer %d", active_layer);
}

int zmk_widget_layer_status_init(struct zmk_widget_layer_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }
    
    // Create container widget for layer display
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 200, 50); // Wide container for horizontal layout
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    
    // Create layer number labels (0-6) in horizontal layout
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        widget->layer_labels[i] = lv_label_create(widget->obj);
        
        // Set large font for visibility - using largest available font
        lv_obj_set_style_text_font(widget->layer_labels[i], &lv_font_montserrat_20, 0);
        
        // Set layer number text
        char layer_text[2];
        snprintf(layer_text, sizeof(layer_text), "%d", i);
        lv_label_set_text(widget->layer_labels[i], layer_text);
        
        // Position horizontally with spacing
        int spacing = 25; // Space between layer numbers
        int start_x = -85; // Start position to center the row
        lv_obj_align(widget->layer_labels[i], LV_ALIGN_CENTER, start_x + (i * spacing), 0);
        
        // Initially all layers are dimmed
        lv_obj_set_style_text_color(widget->layer_labels[i], lv_color_make(80, 80, 80), 0);
        lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_50, 0);
    }
    
    // Layer 0 is active by default
    lv_obj_set_style_text_color(widget->layer_labels[0], lv_color_white(), 0);
    lv_obj_set_style_text_opa(widget->layer_labels[0], LV_OPA_COVER, 0);
    
    LOG_INF("Layer status widget initialized with %d layers", MAX_LAYER_DISPLAY);
    return 0;
}

void zmk_widget_layer_status_update(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd) {
    update_layer_display(widget, kbd);
}

lv_obj_t *zmk_widget_layer_status_obj(struct zmk_widget_layer_status *widget) {
    return widget ? widget->obj : NULL;
}