/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "system_settings_widget.h"
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

int zmk_widget_system_settings_init(struct zmk_widget_system_settings *widget, lv_obj_t *parent) {
    // Create container for system settings screen - FULL SCREEN OVERLAY
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, LV_PCT(100), LV_PCT(100));

    // Make background completely opaque (no transparency)
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, LV_PART_MAIN);  // Fully opaque
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    // Position at (0,0) to cover entire parent
    lv_obj_set_pos(widget->obj, 0, 0);

    // Move to top of z-order so it covers everything
    lv_obj_move_foreground(widget->obj);

    // Title label - at top of screen
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "System Settings");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(widget->title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 30);

    // --- Bootloader Button (visual only, no interaction yet) ---
    lv_obj_t *bootloader_box = lv_label_create(widget->obj);
    lv_label_set_text(bootloader_box, "[ Enter Bootloader ]");
    lv_obj_set_style_text_color(bootloader_box, lv_color_hex(0x2196F3), LV_PART_MAIN);
    lv_obj_set_style_text_align(bootloader_box, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(bootloader_box, LV_ALIGN_CENTER, 0, -40);

    // --- Reset Button (visual only, no interaction yet) ---
    lv_obj_t *reset_box = lv_label_create(widget->obj);
    lv_label_set_text(reset_box, "[ System Reset ]");
    lv_obj_set_style_text_color(reset_box, lv_color_hex(0xF44336), LV_PART_MAIN);
    lv_obj_set_style_text_align(reset_box, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(reset_box, LV_ALIGN_CENTER, 0, 30);

    // Instruction text at bottom
    lv_obj_t *instruction_label = lv_label_create(widget->obj);
    lv_label_set_text(instruction_label, "Swipe up to return");
    lv_obj_set_style_text_color(instruction_label, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(instruction_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(instruction_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("System settings widget initialized - text-based buttons (no click events yet)");
    return 0;
}

void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget) {
    if (widget && widget->obj) {
        // Move to foreground to ensure it covers everything
        lv_obj_move_foreground(widget->obj);
        // Make visible
        lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_DBG("System settings screen shown - moved to foreground");
    }
}

void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget) {
    if (widget && widget->obj) {
        // Hide the overlay
        lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_DBG("System settings screen hidden");
    }
}
