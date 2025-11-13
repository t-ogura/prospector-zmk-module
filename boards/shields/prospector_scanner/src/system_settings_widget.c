/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "system_settings_widget.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Forward declarations for event handlers
static void bootloader_button_event_handler(lv_event_t *e);
static void reset_button_event_handler(lv_event_t *e);

// Lazy initialization - only store parent, don't create UI yet
void zmk_widget_system_settings_init_lazy(struct zmk_widget_system_settings *widget, lv_obj_t *parent) {
    widget->obj = NULL;  // Not created yet
    widget->parent_screen = parent;
    widget->title_label = NULL;
    widget->bootloader_button = NULL;
    widget->reset_button = NULL;
    widget->instruction_label = NULL;
    LOG_INF("System settings widget lazy init - UI will be created on first show");
}

// Create the actual UI (called on first show)
static void create_system_settings_ui(struct zmk_widget_system_settings *widget) {
    if (widget->obj) {
        return;  // Already created
    }

    LOG_INF("Creating system settings UI on demand");

    // Create container for system settings screen - FULL SCREEN OVERLAY
    widget->obj = lv_obj_create(widget->parent_screen);
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

    // --- Bootloader Button (real button widget) ---
    widget->bootloader_button = lv_btn_create(widget->obj);
    lv_obj_set_size(widget->bootloader_button, 180, 50);  // Larger, more touchable
    lv_obj_align(widget->bootloader_button, LV_ALIGN_CENTER, 0, -40);

    // Style the button
    lv_obj_set_style_bg_color(widget->bootloader_button, lv_color_hex(0x2196F3), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->bootloader_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->bootloader_button, 8, LV_PART_MAIN);

    // Add label to button
    lv_obj_t *bootloader_label = lv_label_create(widget->bootloader_button);
    lv_label_set_text(bootloader_label, "Enter Bootloader");
    lv_obj_set_style_text_color(bootloader_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(bootloader_label);

    // Add click event
    lv_obj_add_event_cb(widget->bootloader_button, bootloader_button_event_handler, LV_EVENT_CLICKED, widget);

    // --- Reset Button (real button widget) ---
    widget->reset_button = lv_btn_create(widget->obj);
    lv_obj_set_size(widget->reset_button, 180, 50);  // Larger, more touchable
    lv_obj_align(widget->reset_button, LV_ALIGN_CENTER, 0, 30);

    // Style the button
    lv_obj_set_style_bg_color(widget->reset_button, lv_color_hex(0xF44336), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->reset_button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->reset_button, 8, LV_PART_MAIN);

    // Add label to button
    lv_obj_t *reset_label = lv_label_create(widget->reset_button);
    lv_label_set_text(reset_label, "System Reset");
    lv_obj_set_style_text_color(reset_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(reset_label);

    // Add click event
    lv_obj_add_event_cb(widget->reset_button, reset_button_event_handler, LV_EVENT_CLICKED, widget);

    // Instruction text at bottom
    widget->instruction_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->instruction_label, "Tap button or swipe up");
    lv_obj_set_style_text_color(widget->instruction_label, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(widget->instruction_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(widget->instruction_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("System settings UI created with interactive buttons");
}

void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget) {
    if (!widget) {
        return;
    }

    // Create UI on first show (lazy initialization)
    if (!widget->obj) {
        create_system_settings_ui(widget);
    }

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

// Bootloader button event handler
static void bootloader_button_event_handler(lv_event_t *e) {
    struct zmk_widget_system_settings *widget = (struct zmk_widget_system_settings *)lv_event_get_user_data(e);

    LOG_WRN("⚠️  BOOTLOADER MODE REQUESTED - Entering bootloader...");

    // Update button appearance to show it was clicked
    if (widget && widget->bootloader_button) {
        // Get the label child of the button
        lv_obj_t *label = lv_obj_get_child(widget->bootloader_button, 0);
        if (label) {
            lv_label_set_text(label, "Entering...");
        }
        // Change button color to green
        lv_obj_set_style_bg_color(widget->bootloader_button, lv_color_hex(0x00FF00), LV_PART_MAIN);
    }

    // Force LVGL update to show the change
    lv_refr_now(NULL);

    // Small delay to let user see the feedback
    k_sleep(K_MSEC(500));

    // Enter bootloader mode
    sys_reboot(SYS_REBOOT_COLD);
}

// Reset button event handler
static void reset_button_event_handler(lv_event_t *e) {
    struct zmk_widget_system_settings *widget = (struct zmk_widget_system_settings *)lv_event_get_user_data(e);

    LOG_WRN("⚠️  SYSTEM RESET REQUESTED - Rebooting...");

    // Update button appearance to show it was clicked
    if (widget && widget->reset_button) {
        // Get the label child of the button
        lv_obj_t *label = lv_obj_get_child(widget->reset_button, 0);
        if (label) {
            lv_label_set_text(label, "Rebooting...");
        }
        // Change button color to green
        lv_obj_set_style_bg_color(widget->reset_button, lv_color_hex(0x00FF00), LV_PART_MAIN);
    }

    // Force LVGL update to show the change
    lv_refr_now(NULL);

    // Small delay to let user see the feedback
    k_sleep(K_MSEC(500));

    // Perform system reset
    sys_reboot(SYS_REBOOT_WARM);
}
