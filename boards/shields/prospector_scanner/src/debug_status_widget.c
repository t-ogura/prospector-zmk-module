/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include "debug_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Debug widget positioned in modifier area (overlapping when no modifiers active)
int zmk_widget_debug_status_init(struct zmk_widget_debug_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    
    // Position in very visible center location (overlaps everything for testing)
    lv_obj_align(widget->obj, LV_ALIGN_CENTER, 0, 0);  // Dead center of screen
    
    // Ensure widget is on top of other elements
    lv_obj_move_foreground(widget->obj);
    
    // Make container visible with background for testing
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_80, 0);  // Semi-transparent background
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x000080), 0);  // Dark blue background
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_COVER, 0);  // Visible border
    lv_obj_set_style_border_color(widget->obj, lv_color_hex(0xFF0000), 0);  // Red border
    lv_obj_set_style_border_width(widget->obj, 2, 0);  // 2px border
    lv_obj_set_style_pad_all(widget->obj, 5, 0);  // Some padding
    
    // Create debug text label
    widget->debug_label = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->debug_label, &lv_font_montserrat_12, 0);  // Smaller font to fit more text
    lv_obj_set_style_text_color(widget->debug_label, lv_color_hex(0xFF00FF), 0); // Magenta for high visibility
    lv_obj_set_style_text_align(widget->debug_label, LV_TEXT_ALIGN_LEFT, 0);  // Left align for better readability
    lv_label_set_text(widget->debug_label, "DEBUG WIDGET VISIBLE");  // Test text to confirm visibility
    lv_obj_center(widget->debug_label);
    
    // Start visible for testing
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
    
    // Force immediate display update
    lv_obj_invalidate(widget->obj);
    
    LOG_DBG("Debug status widget initialized at modifier position");
    return 0;
}

lv_obj_t *zmk_widget_debug_status_obj(struct zmk_widget_debug_status *widget) {
    return widget->obj;
}

void zmk_widget_debug_status_set_text(struct zmk_widget_debug_status *widget, const char *text) {
    if (!widget) {
        LOG_ERR("Debug widget is NULL for text set!");
        return;
    }
    if (!widget->debug_label) {
        LOG_ERR("Debug widget label is NULL!");
        return;
    }
    if (!text) {
        LOG_WRN("Debug widget text is NULL!");
        return;
    }
    
    lv_label_set_text(widget->debug_label, text);
    LOG_INF("📝 Debug widget text updated: '%s'", text);
    
    // Force redraw after text change
    lv_obj_invalidate(widget->obj);
}

void zmk_widget_debug_status_set_visible(struct zmk_widget_debug_status *widget, bool visible) {
    if (!widget) {
        LOG_ERR("Debug widget is NULL!");
        return;
    }
    if (!widget->obj) {
        LOG_ERR("Debug widget obj is NULL!");
        return;
    }
    
    if (visible) {
        lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_INF("🟢 Debug widget set to VISIBLE");
    } else {
        lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_INF("🔴 Debug widget set to HIDDEN");
    }
}