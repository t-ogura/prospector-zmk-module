/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "system_settings_widget.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Forward declarations
static void bootloader_btn_event_cb(lv_event_t *e);
static void reset_btn_event_cb(lv_event_t *e);

// Button event handlers
static void bootloader_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        LOG_INF("🔄 Bootloader button clicked - entering bootloader mode");

        // Enter bootloader mode (UF2 bootloader)
        // Set GPREGRET register to indicate bootloader request
        NRF_POWER->GPREGRET = 0x57; // Magic value for bootloader entry

        // Small delay to ensure log is printed
        k_msleep(100);

        // Perform system reset to enter bootloader
        sys_reboot(SYS_REBOOT_WARM);
    }
}

static void reset_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        LOG_INF("🔄 Reset button clicked - performing system reset");

        // Small delay to ensure log is printed
        k_msleep(100);

        // Perform normal system reset
        sys_reboot(SYS_REBOOT_WARM);
    }
}

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

    // --- Bootloader Button ---
    widget->bootloader_btn = lv_obj_create(widget->obj);
    lv_obj_set_size(widget->bootloader_btn, 180, 50);
    lv_obj_align(widget->bootloader_btn, LV_ALIGN_CENTER, 0, -40);

    // Button style - blue with rounded corners
    lv_obj_set_style_bg_color(widget->bootloader_btn, lv_color_hex(0x2196F3), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->bootloader_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->bootloader_btn, 15, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(widget->bootloader_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->bootloader_btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(widget->bootloader_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(widget->bootloader_btn, LV_OBJ_FLAG_SCROLLABLE);

    // Button label
    lv_obj_t *bootloader_label = lv_label_create(widget->bootloader_btn);
    lv_label_set_text(bootloader_label, "Enter Bootloader");
    lv_obj_set_style_text_color(bootloader_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(bootloader_label);

    // Add event handler
    lv_obj_add_event_cb(widget->bootloader_btn, bootloader_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // --- Reset Button ---
    widget->reset_btn = lv_obj_create(widget->obj);
    lv_obj_set_size(widget->reset_btn, 180, 50);
    lv_obj_align(widget->reset_btn, LV_ALIGN_CENTER, 0, 30);

    // Button style - red with rounded corners
    lv_obj_set_style_bg_color(widget->reset_btn, lv_color_hex(0xF44336), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->reset_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->reset_btn, 15, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(widget->reset_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->reset_btn, 0, LV_PART_MAIN);
    lv_obj_add_flag(widget->reset_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(widget->reset_btn, LV_OBJ_FLAG_SCROLLABLE);

    // Button label
    lv_obj_t *reset_label = lv_label_create(widget->reset_btn);
    lv_label_set_text(reset_label, "System Reset");
    lv_obj_set_style_text_color(reset_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(reset_label);

    // Add event handler
    lv_obj_add_event_cb(widget->reset_btn, reset_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Instruction text at bottom
    lv_obj_t *instruction_label = lv_label_create(widget->obj);
    lv_label_set_text(instruction_label, "Swipe up to return");
    lv_obj_set_style_text_color(instruction_label, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(instruction_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(instruction_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("System settings widget initialized - with bootloader and reset buttons");
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
