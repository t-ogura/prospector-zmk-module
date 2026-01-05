/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zmk/status_scanner.h>
#include "wpm_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

void zmk_widget_wpm_status_update(struct zmk_widget_wpm_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !widget->obj || !kbd) {
        return;
    }
    
    uint8_t wpm_value = kbd->data.wpm_value;
    uint32_t current_time = k_uptime_get_32();
    
    // Update last activity time when we receive non-zero WPM
    if (wpm_value > 0) {
        widget->last_activity_time = current_time;
    }
    
    // Check for WPM timeout (2 minutes of no activity = WPM should be 0)
    #define WPM_TIMEOUT_MS 120000  // 2 minutes
    if (widget->last_activity_time > 0 && 
        (current_time - widget->last_activity_time) > WPM_TIMEOUT_MS &&
        wpm_value > 0) {
        // Force WPM to 0 after timeout
        wpm_value = 0;
        LOG_INF("WPM forced to 0 after %d seconds of inactivity", (int)((current_time - widget->last_activity_time) / 1000));
    }
    
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
    widget->last_activity_time = 0;
}

/**
 * LVGL 9 FIX: NO CONTAINER - Create all elements directly on parent
 * Widget positioned at TOP_LEFT with x=10, y=50 in scanner_display.c
 */
int zmk_widget_wpm_status_init(struct zmk_widget_wpm_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }

    widget->parent = parent;

    // Position offsets from TOP_LEFT
    const int X_OFFSET = 10;
    const int Y_OFFSET = 50;

    // Create WPM title label (small font) - created directly on parent
    widget->wpm_title_label = lv_label_create(parent);
    lv_obj_align(widget->wpm_title_label, LV_ALIGN_TOP_LEFT, X_OFFSET, Y_OFFSET);
    lv_label_set_text(widget->wpm_title_label, "WPM");
    lv_obj_set_style_text_font(widget->wpm_title_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(widget->wpm_title_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);

    // Create WPM value label (normal font) - created directly on parent
    widget->wpm_value_label = lv_label_create(parent);
    lv_obj_align(widget->wpm_value_label, LV_ALIGN_TOP_LEFT, X_OFFSET, Y_OFFSET + 12);
    lv_label_set_text(widget->wpm_value_label, "0");
    lv_obj_set_style_text_font(widget->wpm_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(widget->wpm_value_label, lv_color_white(), 0);

    // Set widget->obj to first element for compatibility
    widget->obj = widget->wpm_title_label;

    // Initialize state
    widget->last_wpm_value = 0;
    widget->last_activity_time = 0;

    LOG_INF("✨ WPM status widget initialized (LVGL9 no-container pattern)");
    return 0;
}

// ========== Dynamic Allocation Functions ==========

struct zmk_widget_wpm_status *zmk_widget_wpm_status_create(lv_obj_t *parent) {
    LOG_DBG("Creating WPM status widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory for widget structure using LVGL's memory allocator
    struct zmk_widget_wpm_status *widget =
        (struct zmk_widget_wpm_status *)lv_malloc(sizeof(struct zmk_widget_wpm_status));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for wpm_status_widget (%d bytes)",
                sizeof(struct zmk_widget_wpm_status));
        return NULL;
    }

    // Zero-initialize the allocated memory
    memset(widget, 0, sizeof(struct zmk_widget_wpm_status));

    // Initialize widget (this creates LVGL objects)
    int ret = zmk_widget_wpm_status_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_free(widget);
        return NULL;
    }

    LOG_DBG("WPM status widget created successfully");
    return widget;
}

void zmk_widget_wpm_status_destroy(struct zmk_widget_wpm_status *widget) {
    LOG_DBG("Destroying WPM status widget (LVGL9 no-container)");

    if (!widget) {
        return;
    }

    // LVGL 9 FIX: Delete each element individually (no container parent)
    if (widget->wpm_value_label) {
        lv_obj_del(widget->wpm_value_label);
        widget->wpm_value_label = NULL;
    }
    if (widget->wpm_title_label) {
        lv_obj_del(widget->wpm_title_label);
        widget->wpm_title_label = NULL;
    }

    widget->obj = NULL;
    widget->parent = NULL;

    // Free the widget structure memory from LVGL heap
    lv_free(widget);
}

lv_obj_t *zmk_widget_wpm_status_obj(struct zmk_widget_wpm_status *widget) {
    return widget ? widget->obj : NULL;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY