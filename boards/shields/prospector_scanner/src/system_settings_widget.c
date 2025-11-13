/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "system_settings_widget.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Button click event handler
static void bootloader_btn_event_cb(lv_event_t *e) {
    LOG_INF("Bootloader button clicked - entering bootloader mode");
    // Set GPREGRET register to signal bootloader entry
    NRF_POWER->GPREGRET = 0x57; // Magic value for bootloader
    sys_reboot(SYS_REBOOT_COLD);
}

static void reset_btn_event_cb(lv_event_t *e) {
    LOG_INF("Reset button clicked - performing system reset");
    sys_reboot(SYS_REBOOT_WARM);
}

int zmk_widget_system_settings_init(struct zmk_widget_system_settings *widget, lv_obj_t *parent) {
    // Lazy initialization - only store parent, don't create UI yet
    widget->obj = NULL;
    widget->title_label = NULL;
    widget->bootloader_btn = NULL;
    widget->bootloader_label = NULL;
    widget->reset_btn = NULL;
    widget->reset_label = NULL;
    widget->parent = parent;  // Store parent for later

    LOG_INF("System settings widget initialized (lazy mode - UI will be created on first show)");
    return 0;
}

// Helper function to create the actual UI (called on first show)
static void create_settings_ui(struct zmk_widget_system_settings *widget) {
    if (widget->obj != NULL) {
        return;  // Already created
    }

    LOG_INF("Creating system settings UI (first show)");

    // Create container for system settings screen - FULL SCREEN OVERLAY
    widget->obj = lv_obj_create(widget->parent);
    lv_obj_set_size(widget->obj, LV_PCT(100), LV_PCT(100));

    // Make background completely opaque (no transparency)
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_SCROLLABLE);

    // Position at (0,0) to cover entire parent
    lv_obj_set_pos(widget->obj, 0, 0);

    // Title label - at top
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "System Settings");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(widget->title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 20);

    // Bootloader button
    widget->bootloader_btn = lv_obj_create(widget->obj);
    lv_obj_set_size(widget->bootloader_btn, 180, 50);
    lv_obj_set_style_bg_color(widget->bootloader_btn, lv_color_hex(0x0066CC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->bootloader_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->bootloader_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->bootloader_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(widget->bootloader_btn, lv_color_hex(0x3399FF), LV_PART_MAIN);
    lv_obj_align(widget->bootloader_btn, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(widget->bootloader_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(widget->bootloader_btn, bootloader_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Bootloader button label
    widget->bootloader_label = lv_label_create(widget->bootloader_btn);
    lv_label_set_text(widget->bootloader_label, "Enter Bootloader");
    lv_obj_set_style_text_color(widget->bootloader_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(widget->bootloader_label);

    // Reset button
    widget->reset_btn = lv_obj_create(widget->obj);
    lv_obj_set_size(widget->reset_btn, 180, 50);
    lv_obj_set_style_bg_color(widget->reset_btn, lv_color_hex(0xCC0000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->reset_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->reset_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->reset_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(widget->reset_btn, lv_color_hex(0xFF3333), LV_PART_MAIN);
    lv_obj_align(widget->reset_btn, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_flag(widget->reset_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(widget->reset_btn, reset_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Reset button label
    widget->reset_label = lv_label_create(widget->reset_btn);
    lv_label_set_text(widget->reset_label, "System Reset");
    lv_obj_set_style_text_color(widget->reset_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(widget->reset_label);

    // Instruction at bottom
    lv_obj_t *instruction = lv_label_create(widget->obj);
    lv_label_set_text(instruction, "Swipe up to return");
    lv_obj_set_style_text_color(instruction, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(instruction, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(instruction, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("System settings UI created successfully");
}

void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget) {
    if (!widget) {
        return;
    }

    // Create UI if not created yet (lazy initialization)
    create_settings_ui(widget);

    if (widget->obj) {
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
