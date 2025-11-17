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
    LOG_INF("🔧 System settings widget init START");

    widget->parent = parent;

    if (!parent) {
        LOG_ERR("❌ CRITICAL: parent is NULL!");
        return -EINVAL;
    }

    // Create UI immediately (not lazy) to avoid work queue blocking issues
    LOG_INF("🔧 Creating settings UI during init (not lazy)");

    // Create container for system settings screen - FULL SCREEN OVERLAY
    widget->obj = lv_obj_create(parent);
    if (!widget->obj) {
        LOG_ERR("❌ CRITICAL: lv_obj_create() returned NULL!");
        return -ENOMEM;
    }

    // Set full screen size
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

    // Buttons are disabled for now (see comment block below)
    widget->bootloader_btn = NULL;
    widget->bootloader_label = NULL;
    widget->reset_btn = NULL;
    widget->reset_label = NULL;

    // Instruction at bottom
    lv_obj_t *instruction = lv_label_create(widget->obj);
    lv_label_set_text(instruction, "Swipe up to return");
    lv_obj_set_style_text_color(instruction, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(instruction, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(instruction, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ System settings UI created successfully during init");
    return 0;
}

// Removed create_settings_ui() - UI is now created directly in init()

void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget) {
    LOG_INF("🔵 zmk_widget_system_settings_show() CALLED");

    if (!widget || !widget->obj) {
        LOG_ERR("⚠️  Widget or widget->obj is NULL, cannot show");
        return;
    }

    // UI already created in init, just show it
    lv_obj_move_foreground(widget->obj);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
    LOG_INF("✅ System settings screen shown");
}

void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget) {
    LOG_INF("🔴 zmk_widget_system_settings_hide() CALLED");

    if (widget && widget->obj) {
        // Hide the overlay
        lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_INF("✅ System settings screen hidden");
    } else {
        LOG_WRN("⚠️  Cannot hide - widget or obj is NULL");
    }
}
