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
    LOG_INF("🔧 System settings widget init START (minimal text-only mode)");

    widget->parent = parent;

    if (!parent) {
        LOG_ERR("❌ CRITICAL: parent is NULL!");
        return -EINVAL;
    }

    // Create container - MINIMAL style to avoid potential LVGL issues
    widget->obj = lv_obj_create(parent);
    if (!widget->obj) {
        LOG_ERR("❌ CRITICAL: lv_obj_create() returned NULL!");
        return -ENOMEM;
    }

    // MINIMAL STYLE: absolute size, simple colors only
    lv_obj_set_size(widget->obj, 240, 280);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);

    // Single centered label - NO alignment helpers, just center
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "System Settings");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(widget->title_label);

    // No buttons
    widget->bootloader_btn = NULL;
    widget->bootloader_label = NULL;
    widget->reset_btn = NULL;
    widget->reset_label = NULL;

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ System settings UI created (text-only, minimal)");
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
