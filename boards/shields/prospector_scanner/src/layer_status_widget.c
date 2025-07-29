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

// Stylish pastel colors for each layer (0-6) - using lv_color_make()
static lv_color_t get_layer_color(int layer) {
    switch (layer) {
        case 0: return lv_color_make(0xFF, 0x9B, 0x9B);  // Layer 0: Soft Coral Pink
        case 1: return lv_color_make(0xFF, 0xD9, 0x3D);  // Layer 1: Sunny Yellow  
        case 2: return lv_color_make(0x6B, 0xCF, 0x7F);  // Layer 2: Mint Green
        case 3: return lv_color_make(0x4D, 0x96, 0xFF);  // Layer 3: Sky Blue
        case 4: return lv_color_make(0xB1, 0x9C, 0xD9);  // Layer 4: Lavender Purple
        case 5: return lv_color_make(0xFF, 0x6B, 0x9D);  // Layer 5: Rose Pink
        case 6: return lv_color_make(0xFF, 0x9F, 0x43);  // Layer 6: Peach Orange
        default: return lv_color_white();
    }
}

static void update_layer_display(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !kbd) {
        return;
    }
    
    uint8_t active_layer = kbd->data.active_layer;
    
    // Update each layer label (0-6) with unique pastel colors
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        if (i == active_layer) {
            // Active layer: bright pastel color, fully opaque
            lv_obj_set_style_text_color(widget->layer_labels[i], get_layer_color(i), 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_COVER, 0);
        } else {
            // Inactive layers: same pastel color but very dim
            lv_obj_set_style_text_color(widget->layer_labels[i], get_layer_color(i), 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_20, 0);
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
    lv_obj_set_size(widget->obj, 200, 60); // Slightly taller for "Layer" label
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    
    // Create stylish "Layer" title label (larger font)
    widget->layer_title = lv_label_create(widget->obj);
    lv_label_set_text(widget->layer_title, "Layer");
    lv_obj_set_style_text_font(widget->layer_title, &lv_font_montserrat_14, 0); // Slightly larger title
    lv_obj_set_style_text_color(widget->layer_title, lv_color_make(160, 160, 160), 0); // Soft gray
    lv_obj_set_style_text_opa(widget->layer_title, LV_OPA_70, 0);
    lv_obj_align(widget->layer_title, LV_ALIGN_TOP_MID, 0, -5); // Above the numbers
    
    // Create layer number labels (0-6) in horizontal layout with pastel colors
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        widget->layer_labels[i] = lv_label_create(widget->obj);
        
        // Set larger font for better visibility  
        lv_obj_set_style_text_font(widget->layer_labels[i], &lv_font_montserrat_22, 0);
        
        // Set layer number text
        char layer_text[2];
        snprintf(layer_text, sizeof(layer_text), "%d", i);
        lv_label_set_text(widget->layer_labels[i], layer_text);
        
        // Position horizontally with spacing
        int spacing = 25; // Space between layer numbers
        int start_x = -85; // Start position to center the row
        lv_obj_align(widget->layer_labels[i], LV_ALIGN_CENTER, start_x + (i * spacing), 5); // Below title
        
        // Initialize with pastel colors (initially dimmed)
        lv_obj_set_style_text_color(widget->layer_labels[i], get_layer_color(i), 0);
        lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_20, 0);
    }
    
    // Layer 0 is active by default with full brightness
    lv_obj_set_style_text_color(widget->layer_labels[0], get_layer_color(0), 0);
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