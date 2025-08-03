/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/logging/log.h>
#include <zmk/status_scanner.h>
#include "wpm_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

void zmk_widget_wpm_status_update(struct zmk_widget_wpm_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !widget->obj || !kbd) {
        return;
    }
    
    uint8_t wpm_value = kbd->data.wpm_value;
    
    // Only update if WPM value changed (reduce display flickering)
    if (wpm_value == widget->last_wpm_value) {
        return;
    }
    
    widget->last_wpm_value = wpm_value;
    
    // Update WPM display with MAX!! for 255  
    if (wpm_value >= 255) {
        lv_label_set_text(widget->wpm_value_label, "MAX!!");
    } else {
        lv_label_set_text_fmt(widget->wpm_value_label, "%d", wpm_value);
    }
    LOG_DBG("WPM widget updated: %d", wpm_value);
}

void zmk_widget_wpm_status_reset(struct zmk_widget_wpm_status *widget) {
    if (!widget || !widget->wpm_value_label) {
        return;
    }
    
    LOG_INF("WPM widget reset - clearing WPM display");
    
    // Reset WPM display to inactive state
    lv_label_set_text(widget->wpm_value_label, "0");
    widget->last_wpm_value = 0;
}

int zmk_widget_wpm_status_init(struct zmk_widget_wpm_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }
    
    // Create container widget - enhanced design with title + value
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 60, 35);  // Larger size for title + value
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);  // No border
    lv_obj_set_style_pad_all(widget->obj, 0, 0);  // No padding
    
    // Create WPM title label (small font) - moved down 3px
    widget->wpm_title_label = lv_label_create(widget->obj);
    lv_obj_align(widget->wpm_title_label, LV_ALIGN_TOP_MID, 0, 3);
    lv_label_set_text(widget->wpm_title_label, "WPM");
    lv_obj_set_style_text_font(widget->wpm_title_label, &lv_font_unscii_8, 0);  // Small font
    lv_obj_set_style_text_color(widget->wpm_title_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);  // Gray text
    lv_obj_set_style_text_align(widget->wpm_title_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // Create WPM value label (normal font)
    widget->wpm_value_label = lv_label_create(widget->obj);
    lv_obj_align(widget->wpm_value_label, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_label_set_text(widget->wpm_value_label, "0");  // Initial inactive state
    lv_obj_set_style_text_font(widget->wpm_value_label, &lv_font_montserrat_16, 0);  // Normal size font
    lv_obj_set_style_text_color(widget->wpm_value_label, lv_color_make(0xFF, 0xFF, 0xFF), 0);  // White text
    lv_obj_set_style_text_align(widget->wpm_value_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // Initialize state
    widget->last_wpm_value = 0;
    
    LOG_INF("Enhanced WPM status widget initialized with title + value layout");
    return 0;
}

lv_obj_t *zmk_widget_wpm_status_obj(struct zmk_widget_wpm_status *widget) {
    return widget ? widget->obj : NULL;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY