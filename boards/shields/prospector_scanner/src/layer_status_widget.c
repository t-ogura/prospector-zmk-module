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

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
#include "display_settings_widget.h"  // For display_settings_get_max_layers_global()
#endif

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
    if (!widget || !kbd || !widget->obj) {
        return;
    }

    uint8_t active_layer = kbd->data.active_layer;
    uint8_t num_layers = widget->visible_layers;

    // Clamp num_layers to valid range
    if (num_layers < 4) num_layers = 4;
    if (num_layers > MAX_LAYER_DISPLAY) num_layers = MAX_LAYER_DISPLAY;

    // Update each layer label with colors
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        // Skip if label doesn't exist
        if (!widget->layer_labels[i]) {
            continue;
        }

        if (i >= num_layers) {
            // Hide layers beyond visible_layers
            lv_obj_add_flag(widget->layer_labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_clear_flag(widget->layer_labels[i], LV_OBJ_FLAG_HIDDEN);

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

    LOG_DBG("Layer display updated: active layer %d (showing %d layers)", active_layer, num_layers);
}

/**
 * LVGL 9 FIX: No container - all labels created directly on parent screen.
 * This avoids the LVGL 9 freeze bug when setting text on labels inside containers.
 * See: LVGL9_CONTAINER_LABEL_FREEZE_BUG.md
 */
int zmk_widget_layer_status_init(struct zmk_widget_layer_status *widget, lv_obj_t *parent, int32_t y_center_offset) {
    if (!widget || !parent) {
        return -1;
    }

    // Store parent and position for later use
    widget->parent = parent;
    widget->y_center_offset = y_center_offset;

    // Get visible layers from global settings (touch version) or Kconfig (non-touch version)
#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    widget->visible_layers = display_settings_get_max_layers_global();
#else
    widget->visible_layers = CONFIG_PROSPECTOR_MAX_LAYERS;
#endif
    if (widget->visible_layers < 4) widget->visible_layers = 4;
    if (widget->visible_layers > MAX_LAYER_DISPLAY) widget->visible_layers = MAX_LAYER_DISPLAY;

    // LVGL 9 FIX: NO CONTAINER - Create labels directly on parent screen
    // Previous code: widget->obj = lv_obj_create(parent); // CAUSES FREEZE IN LVGL 9

    // Create stylish "Layer" title label directly on parent
    widget->layer_title = lv_label_create(parent);
    lv_label_set_text(widget->layer_title, "Layer");
    lv_obj_set_style_text_font(widget->layer_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(widget->layer_title, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_opa(widget->layer_title, LV_OPA_70, 0);
    // Position: center horizontally, above the layer numbers
    lv_obj_align(widget->layer_title, LV_ALIGN_CENTER, 0, y_center_offset - 25);

    // Set widget->obj to layer_title for compatibility with zmk_widget_layer_status_obj()
    widget->obj = widget->layer_title;

    uint8_t num_layers = widget->visible_layers;

    // Create layer number labels directly on parent
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        widget->layer_labels[i] = lv_label_create(parent);

        // Set larger font for better visibility
        lv_obj_set_style_text_font(widget->layer_labels[i], &lv_font_montserrat_28, 0);

        // Set layer number text (support 0-9)
        char layer_text[3];
        snprintf(layer_text, sizeof(layer_text), "%d", i);
        lv_label_set_text(widget->layer_labels[i], layer_text);

        // Dynamic positioning - center the row based on visible layer count
        int spacing = (num_layers <= 4) ? 35 :
                      (num_layers <= 7) ? 25 :
                                          18;

        int total_width = (num_layers - 1) * spacing;
        int start_x = -(total_width / 2);

        // Position relative to screen center with y_center_offset
        lv_obj_align(widget->layer_labels[i], LV_ALIGN_CENTER, start_x + (i * spacing), y_center_offset + 5);

        // Initialize with much darker gray color (barely visible)
        lv_obj_set_style_text_color(widget->layer_labels[i], lv_color_make(40, 40, 40), 0);
        lv_obj_set_style_text_opa(widget->layer_labels[i], LV_OPA_30, 0);

        // Hide layers beyond visible count
        if (i >= num_layers) {
            lv_obj_add_flag(widget->layer_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Layer 0 is active by default with full brightness
    lv_obj_set_style_text_color(widget->layer_labels[0], get_layer_color(0), 0);
    lv_obj_set_style_text_opa(widget->layer_labels[0], LV_OPA_COVER, 0);

    LOG_INF("✨ Layer widget initialized (LVGL9 no-container): %d layers visible (0-%d)",
            num_layers, num_layers - 1);
    return 0;
}

void zmk_widget_layer_status_set_visible_layers(struct zmk_widget_layer_status *widget, uint8_t count) {
    if (!widget || !widget->layer_title) {
        return;
    }

    // Clamp to valid range
    if (count < 4) count = 4;
    if (count > MAX_LAYER_DISPLAY) count = MAX_LAYER_DISPLAY;

    widget->visible_layers = count;

    // Recalculate spacing and positions for visible layers
    int spacing = (count <= 4) ? 35 :
                  (count <= 7) ? 25 :
                                 18;

    int total_width = (count - 1) * spacing;
    int start_x = -(total_width / 2);

    // Update all layer label positions and visibility (LVGL 9: use stored y_center_offset)
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        if (!widget->layer_labels[i]) {
            continue;
        }

        if (i >= count) {
            lv_obj_add_flag(widget->layer_labels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(widget->layer_labels[i], LV_OBJ_FLAG_HIDDEN);
            // LVGL 9 FIX: Position relative to screen center using stored offset
            lv_obj_align(widget->layer_labels[i], LV_ALIGN_CENTER,
                         start_x + (i * spacing), widget->y_center_offset + 5);
        }
    }

    LOG_INF("🔧 Layer widget: now showing %d layers", count);
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

// ========== Dynamic Allocation Functions ==========

/**
 * LVGL 9 FIX: y_center_offset specifies vertical position relative to screen center.
 * This replaces the previous pattern of creating widget then calling lv_obj_align().
 */
struct zmk_widget_layer_status *zmk_widget_layer_status_create(lv_obj_t *parent, int32_t y_center_offset) {
    LOG_DBG("Creating layer status widget (LVGL9 no-container, y_offset=%d)", y_center_offset);

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory for widget structure using LVGL's memory allocator
    // LVGL 9 API: lv_malloc (was lv_mem_alloc in LVGL 8)
    struct zmk_widget_layer_status *widget =
        (struct zmk_widget_layer_status *)lv_malloc(sizeof(struct zmk_widget_layer_status));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for layer_status_widget (%d bytes)",
                sizeof(struct zmk_widget_layer_status));
        return NULL;
    }

    // Zero-initialize the allocated memory
    memset(widget, 0, sizeof(struct zmk_widget_layer_status));

    // Initialize widget with position (LVGL 9: no separate align call needed)
    int ret = zmk_widget_layer_status_init(widget, parent, y_center_offset);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_free(widget);
        return NULL;
    }

    LOG_DBG("Layer status widget created successfully");
    return widget;
}

void zmk_widget_layer_status_destroy(struct zmk_widget_layer_status *widget) {
    LOG_DBG("Destroying layer status widget (LVGL9 no-container)");

    if (!widget) {
        return;
    }

    // LVGL 9 FIX: Delete each label individually (no container parent)
    // Delete layer number labels
    for (int i = 0; i < MAX_LAYER_DISPLAY; i++) {
        if (widget->layer_labels[i]) {
            lv_obj_del(widget->layer_labels[i]);
            widget->layer_labels[i] = NULL;
        }
    }

    // Delete title label
    if (widget->layer_title) {
        lv_obj_del(widget->layer_title);
        widget->layer_title = NULL;
    }

    widget->obj = NULL;
    widget->parent = NULL;

    // Free the widget structure memory from LVGL heap
    // LVGL 9 API: lv_free (was lv_mem_free in LVGL 8)
    lv_free(widget);
}