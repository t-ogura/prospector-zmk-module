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

// Stylish pastel colors for 0-9 layers (10 total) - full support
static lv_color_t get_layer_color(int layer) {
    switch (layer) {
        case 0: return lv_color_make(0xFF, 0x9B, 0x9B);  // Layer 0: Soft Coral Pink
        case 1: return lv_color_make(0xFF, 0xD9, 0x3D);  // Layer 1: Sunny Yellow  
        case 2: return lv_color_make(0x6B, 0xCF, 0x7F);  // Layer 2: Mint Green
        case 3: return lv_color_make(0x4D, 0x96, 0xFF);  // Layer 3: Sky Blue
        case 4: return lv_color_make(0xB1, 0x9C, 0xD9);  // Layer 4: Lavender Purple
        case 5: return lv_color_make(0xFF, 0x6B, 0x9D);  // Layer 5: Rose Pink
        case 6: return lv_color_make(0xFF, 0x9F, 0x43);  // Layer 6: Peach Orange
        case 7: return lv_color_make(0x87, 0xCE, 0xEB);  // Layer 7: Light Sky Blue
        case 8: return lv_color_make(0xF0, 0xE6, 0x8C);  // Layer 8: Light Khaki
        case 9: return lv_color_make(0xDD, 0xA0, 0xDD);  // Layer 9: Plum
        default: return lv_color_white(); // Fallback for undefined layers
    }
}

static void update_layer_display(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !kbd) {
        return;
    }
    
    uint8_t active_layer = kbd->data.active_layer;
    
    // Update each layer label (0-6) with colors
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        if (i == active_layer) {
            // Active layer: bright pastel color, fully opaque
            lv_obj_set_style_text_color(widget->layer_labels[i], get_layer_color(i), 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_COVER, 0);
        } else {
            // Inactive layers: much darker gray, barely visible but still readable
            lv_obj_set_style_text_color(widget->layer_labels[i], lv_color_make(40, 40, 40), 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_30, 0);
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
    
    // Create stylish "Layer" title label (smaller font)
    widget->layer_title = lv_label_create(widget->obj);
    lv_label_set_text(widget->layer_title, "Layer");
    lv_obj_set_style_text_font(widget->layer_title, &lv_font_montserrat_12, 0); // Smaller title font
    lv_obj_set_style_text_color(widget->layer_title, lv_color_make(160, 160, 160), 0); // Soft gray
    lv_obj_set_style_text_opa(widget->layer_title, LV_OPA_70, 0);
    lv_obj_align(widget->layer_title, LV_ALIGN_TOP_MID, 0, -5); // Above the numbers
    
    // Create layer number labels (0-9) in horizontal layout with dynamic centering
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        widget->layer_labels[i] = lv_label_create(widget->obj);
        
        // Set larger font for better visibility  
        lv_obj_set_style_text_font(widget->layer_labels[i], &lv_font_montserrat_28, 0);  // Large numbers (enabled via config)
        
        // Set layer number text (support 0-9)
        char layer_text[3];  // Support double digits
        snprintf(layer_text, sizeof(layer_text), "%d", i);
        lv_label_set_text(widget->layer_labels[i], layer_text);
        
        // Dynamic positioning - center the row based on actual layer count
        int spacing = (MAX_LAYER_DISPLAY <= 4) ? 35 :    // Wide spacing for 4 layers
                      (MAX_LAYER_DISPLAY <= 7) ? 25 :    // Medium spacing for 5-7 layers
                                                 18;     // Tight spacing for 8-10 layers
        
        // Calculate start position to center the entire row
        int total_width = (MAX_LAYER_DISPLAY - 1) * spacing;
        int start_x = -(total_width / 2);
        
        lv_obj_align(widget->layer_labels[i], LV_ALIGN_CENTER, start_x + (i * spacing), 5); // Below title
        
        // Initialize with much darker gray color (barely visible)
        lv_obj_set_style_text_color(widget->layer_labels[i], lv_color_make(40, 40, 40), 0);
        lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_30, 0);
    }
    
    // Layer 0 is active by default with full brightness
    lv_obj_set_style_text_color(widget->layer_labels[0], get_layer_color(0), 0);
    lv_obj_set_style_text_opa(widget->layer_labels[0], LV_OPA_COVER, 0);
    
    LOG_INF("✨ Layer widget initialized: %d layers (0-%d) with dynamic centering", MAX_LAYER_DISPLAY, MAX_LAYER_DISPLAY-1);
    return 0;
}

void zmk_widget_layer_status_update(struct zmk_widget_layer_status *widget, struct zmk_keyboard_status *kbd) {
    update_layer_display(widget, kbd);
}

void zmk_widget_layer_status_reset(struct zmk_widget_layer_status *widget) {
    if (!widget || !widget->layer_labels[0]) {
        return;
    }
    
    LOG_INF("Layer widget reset - resetting to layer 0");
    
    // Reset all layers to inactive state (dark gray)
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        if (widget->layer_labels[i]) {
            lv_obj_set_style_text_color(widget->layer_labels[i], lv_color_make(40, 40, 40), 0);
            lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_30, 0);
        }
    }
    
    // Set layer 0 as active (default state)
    if (widget->layer_labels[0]) {
        lv_obj_set_style_text_color(widget->layer_labels[0], lv_color_make(0xFF, 0x9B, 0x9B), 0); // Soft Coral Pink
        lv_obj_set_style_text_opa(widget->layer_labels[0], LV_OPA_COVER, 0);
    }
}

lv_obj_t *zmk_widget_layer_status_obj(struct zmk_widget_layer_status *widget) {
    return widget ? widget->obj : NULL;
}