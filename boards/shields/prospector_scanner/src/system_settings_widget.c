/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "system_settings_widget.h"
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

int zmk_widget_system_settings_init(struct zmk_widget_system_settings *widget, lv_obj_t *parent) {
    // Create container for system settings screen
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    // Title label - centered on screen
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "System Settings\n\nSwipe up to return");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(widget->title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(widget->title_label, LV_ALIGN_CENTER, 0, 0);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("System settings widget initialized (simple version - title only)");
    return 0;
}

void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget) {
    if (widget && widget->obj) {
        lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_DBG("System settings screen shown");
    }
}

void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget) {
    if (widget && widget->obj) {
        lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_DBG("System settings screen hidden");
    }
}
