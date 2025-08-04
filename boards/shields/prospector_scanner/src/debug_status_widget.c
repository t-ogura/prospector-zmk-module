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
    
    // Position in modifier widget area (bottom center)
    lv_obj_align(widget->obj, LV_ALIGN_BOTTOM_MID, 0, -70);
    
    // Make container transparent
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    
    // Create debug text label
    widget->debug_label = lv_label_create(widget->obj);
    lv_obj_set_style_text_font(widget->debug_label, &lv_font_montserrat_12, 0);  // Use available font
    lv_obj_set_style_text_color(widget->debug_label, lv_color_hex(0xFFFFFF), 0); // White text
    lv_obj_set_style_text_align(widget->debug_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(widget->debug_label, "DEBUG READY");  // Test text to confirm visibility
    lv_obj_center(widget->debug_label);
    
    // Start visible for testing
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
    
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