/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "system_settings_widget.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// ========== Button Event Handlers ==========

static void bootloader_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    // Log ALL events for debugging
    const char *event_name = "UNKNOWN";
    switch (code) {
        case LV_EVENT_PRESSED: event_name = "PRESSED"; break;
        case LV_EVENT_PRESSING: event_name = "PRESSING"; break;
        case LV_EVENT_PRESS_LOST: event_name = "PRESS_LOST"; break;
        case LV_EVENT_SHORT_CLICKED: event_name = "SHORT_CLICKED"; break;
        case LV_EVENT_LONG_PRESSED: event_name = "LONG_PRESSED"; break;
        case LV_EVENT_LONG_PRESSED_REPEAT: event_name = "LONG_PRESSED_REPEAT"; break;
        case LV_EVENT_CLICKED: event_name = "CLICKED"; break;
        case LV_EVENT_RELEASED: event_name = "RELEASED"; break;
        default: break;
    }

    LOG_INF("🔵 Bootloader button: %s (code=%d)", event_name, code);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        LOG_INF("🔵🔵🔵 Bootloader button ACTIVATED - entering bootloader mode NOW!");

        // Reboot with UF2 bootloader magic value
        // CONFIG_NRF_STORE_REBOOT_TYPE_GPREGRET automatically sets GPREGRET to this value
        // See: zmk/app/include/dt-bindings/zmk/reset.h - RST_UF2 = 0x57
        // Adafruit bootloader checks for DFU_MAGIC_UF2_RESET = 0x57
        LOG_INF("🔵 Rebooting with magic value 0x57 for UF2 bootloader entry...");

        sys_reboot(0x57);  // Pass magic value directly - Zephyr sets GPREGRET automatically
    }
}

static void reset_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    // Log ALL events for debugging
    const char *event_name = "UNKNOWN";
    switch (code) {
        case LV_EVENT_PRESSED: event_name = "PRESSED"; break;
        case LV_EVENT_PRESSING: event_name = "PRESSING"; break;
        case LV_EVENT_PRESS_LOST: event_name = "PRESS_LOST"; break;
        case LV_EVENT_SHORT_CLICKED: event_name = "SHORT_CLICKED"; break;
        case LV_EVENT_LONG_PRESSED: event_name = "LONG_PRESSED"; break;
        case LV_EVENT_LONG_PRESSED_REPEAT: event_name = "LONG_PRESSED_REPEAT"; break;
        case LV_EVENT_CLICKED: event_name = "CLICKED"; break;
        case LV_EVENT_RELEASED: event_name = "RELEASED"; break;
        default: break;
    }

    LOG_INF("🔴 Reset button: %s (code=%d)", event_name, code);

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        LOG_INF("🔴🔴🔴 Reset button ACTIVATED - performing system reset NOW!");
        LOG_INF("🔴 Performing warm reboot...");

        // Perform warm reboot (normal restart)
        sys_reboot(SYS_REBOOT_WARM);
    }
}

// ========== Helper: Create Styled Button ==========

static lv_obj_t *create_styled_button(lv_obj_t *parent, const char *text,
                                       lv_color_t bg_color, lv_color_t bg_color_pressed,
                                       int x_offset, int y_offset) {
    // Create button using lv_btn (proper LVGL v7 button widget)
    lv_obj_t *btn = lv_btn_create(parent);
    if (!btn) {
        LOG_ERR("Failed to create button");
        return NULL;
    }

    // Set button size and position
    lv_obj_set_size(btn, 200, 60);
    lv_obj_align(btn, LV_ALIGN_CENTER, x_offset, y_offset);

    // Style: Normal state (LV_STATE_DEFAULT)
    lv_obj_set_style_bg_color(btn, bg_color, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_lighten(bg_color, 60), LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(btn, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(btn, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, LV_STATE_DEFAULT);

    // Style: Pressed state (visual feedback)
    lv_obj_set_style_bg_color(btn, bg_color_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 5, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_50, LV_STATE_PRESSED);

    // Create label on button
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_STATE_DEFAULT);
    lv_obj_center(label);

    return btn;
}

// ========== Widget Initialization ==========

int zmk_widget_system_settings_init(struct zmk_widget_system_settings *widget, lv_obj_t *parent) {
    LOG_INF("🔧 System settings widget init START");

    if (!parent) {
        LOG_ERR("❌ Parent is NULL!");
        return -EINVAL;
    }

    widget->parent = parent;

    // Create full-screen container
    widget->obj = lv_obj_create(parent);
    if (!widget->obj) {
        LOG_ERR("❌ Failed to create container!");
        return -ENOMEM;
    }

    // Container styling (dark background)
    lv_obj_set_size(widget->obj, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(widget->obj, 0, 0);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x0A0A0A), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(widget->obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(widget->obj, 0, LV_STATE_DEFAULT);

    LOG_INF("✅ Container created and styled");

    // Title label (much higher position - more separation from buttons)
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "Quick Actions");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(widget->title_label, &lv_font_montserrat_20, LV_STATE_DEFAULT);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 20);  // Y: 40→20 (higher)

    LOG_INF("✅ Title label created");

    // Bootloader button (blue theme) - buttons closer together
    LOG_INF("Creating Bootloader button...");
    widget->bootloader_btn = create_styled_button(
        widget->obj,
        "Enter Bootloader",
        lv_color_hex(0x4A90E2),  // Normal: Sky blue
        lv_color_hex(0x357ABD),  // Pressed: Darker blue
        0, -15  // Y: -5→-15 (moved up)
    );

    if (!widget->bootloader_btn) {
        LOG_ERR("❌ Failed to create bootloader button");
        lv_obj_del(widget->obj);
        return -ENOMEM;
    }

    // Register ALL event types to debug touch detection
    lv_obj_add_event_cb(widget->bootloader_btn, bootloader_btn_event_cb, LV_EVENT_ALL, NULL);

    LOG_INF("✅ Bootloader button created with event handler");

    // Reset button (red theme) - buttons closer together
    LOG_INF("Creating Reset button...");
    widget->reset_btn = create_styled_button(
        widget->obj,
        "System Reset",
        lv_color_hex(0xE24A4A),  // Normal: Soft red
        lv_color_hex(0xC93A3A),  // Pressed: Darker red
        0, 55   // Y: 65→55 (moved up)
    );

    if (!widget->reset_btn) {
        LOG_ERR("❌ Failed to create reset button");
        lv_obj_del(widget->obj);
        return -ENOMEM;
    }

    // Register ALL event types to debug touch detection
    lv_obj_add_event_cb(widget->reset_btn, reset_btn_event_cb, LV_EVENT_ALL, NULL);

    LOG_INF("✅ Reset button created with event handler");

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ System settings widget initialized with styled buttons");
    return 0;
}

// ========== Dynamic Allocation Functions ==========

struct zmk_widget_system_settings *zmk_widget_system_settings_create(lv_obj_t *parent) {
    LOG_DBG("Creating system settings widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory using LVGL's allocator
    struct zmk_widget_system_settings *widget =
        (struct zmk_widget_system_settings *)lv_mem_alloc(sizeof(struct zmk_widget_system_settings));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for system_settings_widget (%d bytes)",
                sizeof(struct zmk_widget_system_settings));
        return NULL;
    }

    // Zero-initialize
    memset(widget, 0, sizeof(struct zmk_widget_system_settings));

    // Initialize widget
    int ret = zmk_widget_system_settings_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_mem_free(widget);
        return NULL;
    }

    LOG_DBG("System settings widget created successfully");
    return widget;
}

void zmk_widget_system_settings_destroy(struct zmk_widget_system_settings *widget) {
    LOG_DBG("Destroying system settings widget (dynamic deallocation)");

    if (!widget) {
        return;
    }

    // Delete container (automatically deletes all children: buttons and labels)
    if (widget->obj) {
        lv_obj_del(widget->obj);
        widget->obj = NULL;
    }

    // Clear all pointers (children already deleted by parent)
    widget->title_label = NULL;
    widget->bootloader_btn = NULL;
    widget->bootloader_label = NULL;
    widget->reset_btn = NULL;
    widget->reset_label = NULL;

    // Free widget memory
    lv_mem_free(widget);
}

// ========== Widget Control Functions ==========

void zmk_widget_system_settings_show(struct zmk_widget_system_settings *widget) {
    LOG_INF("📱 Showing system settings widget");

    if (!widget || !widget->obj) {
        LOG_ERR("⚠️  Widget or widget->obj is NULL, cannot show");
        return;
    }

    // Show the screen
    lv_obj_move_foreground(widget->obj);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ System settings screen shown");
}

void zmk_widget_system_settings_hide(struct zmk_widget_system_settings *widget) {
    LOG_INF("🚫 Hiding system settings widget");

    if (!widget || !widget->obj) {
        LOG_WRN("⚠️  Cannot hide - widget or obj is NULL");
        return;
    }

    // Hide the screen
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ System settings screen hidden");
}
