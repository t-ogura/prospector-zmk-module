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

/**
 * @brief WPM status widget structure
 * 
 * YADS-style WPM display widget for showing typing speed
 */
struct zmk_widget_wpm_status {
    lv_obj_t *obj;
    lv_obj_t *wpm_title_label;    // "WPM" label (small font)
    lv_obj_t *wpm_value_label;    // Number value (normal font)
    uint8_t last_wpm_value;
    uint32_t last_activity_time;  // Timestamp of last non-zero WPM activity
};

/**
 * @brief Initialize WPM status widget
 * 
 * @param widget Widget structure to initialize
 * @param parent Parent LVGL object
 * @return 0 on success, negative error code on failure
 */
int zmk_widget_wpm_status_init(struct zmk_widget_wpm_status *widget, lv_obj_t *parent);

/**
 * @brief Update WPM status display
 * 
 * @param widget Widget to update
 * @param kbd Keyboard status data containing WPM value
 */
void zmk_widget_wpm_status_update(struct zmk_widget_wpm_status *widget, struct zmk_keyboard_status *kbd);

/**
 * @brief Reset WPM status display to default state
 * 
 * @param widget Widget to reset
 */
void zmk_widget_wpm_status_reset(struct zmk_widget_wpm_status *widget);

/**
 * @brief Get widget LVGL object
 * 
 * @param widget Widget structure
 * @return LVGL object pointer or NULL
 */
lv_obj_t *zmk_widget_wpm_status_obj(struct zmk_widget_wpm_status *widget);

#ifdef __cplusplus
}
#endif