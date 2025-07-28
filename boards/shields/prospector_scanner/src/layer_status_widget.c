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

// Stylish pastel colors for each layer (0-6)
static const lv_color_t layer_colors[MAX_LAYER_DISPLAY] = {
    {.full = 0xFF9B9B},  // Layer 0: Soft Coral Pink
    {.full = 0xFFD93D},  // Layer 1: Sunny Yellow  
    {.full = 0x6BCF7F},  // Layer 2: Mint Green
    {.full = 0x4D96FF},  // Layer 3: Sky Blue
    {.full = 0xB19CD9},  // Layer 4: Lavender Purple
    {.full = 0xFF6B9D},  // Layer 5: Rose Pink
    {.full = 0xFF9F43}   // Layer 6: Peach Orange
};

static void update_layer_display(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !kbd) {
        return;
    }
    
    uint8_t active_layer = kbd->data.active_layer;
    
    // Update each layer label (0-6) with unique pastel colors
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        if (i == active_layer) {
            // Active layer: bright pastel color, fully opaque
            lv_obj_set_style_text_color(widget->layer_labels[i], layer_colors[i], 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_COVER, 0);
        } else {
            // Inactive layers: same pastel color but very dim
            lv_obj_set_style_text_color(widget->layer_labels[i], layer_colors[i], 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_20, 0);
        }
    }
    
    LOG_DBG("Layer display updated: active layer %d with color 0x%06X", 
            active_layer, layer_colors[active_layer].full);
}

int zmk_widget_layer_status_init(struct zmk_widget_layer_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }
    
    // Create container widget for layer display
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 200, 60); // Slightly taller for "Layer" label
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    
    // Create stylish "Layer" title label
    widget->layer_title = lv_label_create(widget->obj);
    lv_label_set_text(widget->layer_title, "Layer");
    lv_obj_set_style_text_font(widget->layer_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widget->layer_title, lv_color_make(160, 160, 160), 0); // Soft gray
    lv_obj_set_style_text_opa(widget->layer_title, LV_OPA_70, 0);
    lv_obj_align(widget->layer_title, LV_ALIGN_TOP_MID, 0, -5); // Above the numbers
    
    // Create layer number labels (0-6) in horizontal layout with pastel colors
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        widget->layer_labels[i] = lv_label_create(widget->obj);
        
        // Set large font for visibility
        lv_obj_set_style_text_font(widget->layer_labels[i], &lv_font_montserrat_20, 0);
        
        // Set layer number text
        char layer_text[2];
        snprintf(layer_text, sizeof(layer_text), "%d", i);
        lv_label_set_text(widget->layer_labels[i], layer_text);
        
        // Position horizontally with spacing
        int spacing = 25; // Space between layer numbers
        int start_x = -85; // Start position to center the row
        lv_obj_align(widget->layer_labels[i], LV_ALIGN_CENTER, start_x + (i * spacing), 5); // Below title
        
        // Initialize with pastel colors (initially dimmed)
        lv_obj_set_style_text_color(widget->layer_labels[i], layer_colors[i], 0);
        lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_20, 0);
    }
    
    // Layer 0 is active by default with full brightness
    lv_obj_set_style_text_color(widget->layer_labels[0], layer_colors[0], 0);
    lv_obj_set_style_text_opa(widget->layer_labels[0], LV_OPA_COVER, 0);
    
    LOG_INF("Stylish layer status widget initialized with %d pastel-colored layers", MAX_LAYER_DISPLAY);
    return 0;
}

void zmk_widget_layer_status_update(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd) {
    update_layer_display(widget, kbd);
}

lv_obj_t *zmk_widget_layer_status_obj(struct zmk_widget_layer_status *widget) {
    return widget ? widget->obj : NULL;
}