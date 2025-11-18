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

    LOG_INF("📐 Display resolution: LV_HOR_RES=%d, LV_VER_RES=%d", LV_HOR_RES, LV_VER_RES);

    // Create full-screen container
    LOG_INF("Creating container object...");
    widget->obj = lv_obj_create(parent);
    if (!widget->obj) {
        LOG_ERR("❌ CRITICAL: lv_obj_create() returned NULL!");
        return -ENOMEM;
    }
    LOG_INF("✅ Container created");

    // Set to full screen size
    LOG_INF("Setting container size and position...");
    lv_obj_set_size(widget->obj, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(widget->obj, 0, 0);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(widget->obj, 0, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    LOG_INF("✅ Container styled");

    // Title label at top
    LOG_INF("Creating title label...");
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "System Settings");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(widget->title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 20);
    LOG_INF("✅ Title label created");

    // ⚠️ BUTTONS DISABLED FOR DEBUGGING - Just draw placeholder rectangles
    LOG_INF("Creating placeholder buttons (NO event handlers)...");

    // Bootloader button placeholder (centered, upper) - NO EVENT HANDLER
    widget->bootloader_btn = lv_obj_create(widget->obj);
    lv_obj_set_size(widget->bootloader_btn, 180, 50);
    lv_obj_align(widget->bootloader_btn, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_bg_color(widget->bootloader_btn, lv_color_hex(0x4A90E2), 0);
    // lv_obj_add_event_cb(widget->bootloader_btn, bootloader_btn_event_cb, LV_EVENT_CLICKED, NULL);  // DISABLED

    widget->bootloader_label = lv_label_create(widget->bootloader_btn);
    lv_label_set_text(widget->bootloader_label, "Enter Bootloader");
    lv_obj_set_style_text_color(widget->bootloader_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(widget->bootloader_label);
    LOG_INF("✅ Bootloader placeholder created");

    // Reset button placeholder (centered, lower) - NO EVENT HANDLER
    widget->reset_btn = lv_obj_create(widget->obj);
    lv_obj_set_size(widget->reset_btn, 180, 50);
    lv_obj_align(widget->reset_btn, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(widget->reset_btn, lv_color_hex(0xE24A4A), 0);
    // lv_obj_add_event_cb(widget->reset_btn, reset_btn_event_cb, LV_EVENT_CLICKED, NULL);  // DISABLED

    widget->reset_label = lv_label_create(widget->reset_btn);
    lv_label_set_text(widget->reset_label, "System Reset");
    lv_obj_set_style_text_color(widget->reset_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(widget->reset_label);
    LOG_INF("✅ Reset placeholder created");

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ System settings UI created (buttons DISABLED for debugging)");
    return 0;
}

// Dynamic allocation: Create widget with memory allocation
struct zmk_widget_system_settings *zmk_widget_system_settings_create(lv_obj_t *parent) {
    LOG_INF("🔷 Creating system settings widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("❌ Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory for widget structure using LVGL's memory allocator
    struct zmk_widget_system_settings *widget =
        (struct zmk_widget_system_settings *)lv_mem_alloc(sizeof(struct zmk_widget_system_settings));
    if (!widget) {
        LOG_ERR("❌ Failed to allocate memory for system_settings_widget (%d bytes)",
                sizeof(struct zmk_widget_system_settings));
        return NULL;
    }

    // Zero-initialize the allocated memory
    memset(widget, 0, sizeof(struct zmk_widget_system_settings));

    LOG_INF("✅ Allocated %d bytes for widget structure from LVGL heap",
            sizeof(struct zmk_widget_system_settings));

    // Initialize widget (this creates LVGL objects)
    int ret = zmk_widget_system_settings_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("❌ Widget initialization failed, freeing memory");
        lv_mem_free(widget);
        return NULL;
    }

    LOG_INF("✅ System settings widget created successfully");
    return widget;
}

// Dynamic deallocation: Destroy widget and free memory
void zmk_widget_system_settings_destroy(struct zmk_widget_system_settings *widget) {
    LOG_INF("🔶 Destroying system settings widget (dynamic deallocation)");

    if (!widget) {
        LOG_WRN("⚠️  Widget is NULL, nothing to destroy");
        return;
    }

    // Delete LVGL objects first (in reverse order of creation)
    if (widget->reset_label) {
        lv_obj_del(widget->reset_label);
        widget->reset_label = NULL;
    }
    if (widget->reset_btn) {
        lv_obj_del(widget->reset_btn);
        widget->reset_btn = NULL;
    }
    if (widget->bootloader_label) {
        lv_obj_del(widget->bootloader_label);
        widget->bootloader_label = NULL;
    }
    if (widget->bootloader_btn) {
        lv_obj_del(widget->bootloader_btn);
        widget->bootloader_btn = NULL;
    }
    if (widget->title_label) {
        lv_obj_del(widget->title_label);
        widget->title_label = NULL;
    }
    if (widget->obj) {
        lv_obj_del(widget->obj);  // This also deletes all children
        widget->obj = NULL;
    }

    LOG_INF("✅ LVGL objects deleted");

    // Free the widget structure memory from LVGL heap
    lv_mem_free(widget);
    LOG_INF("✅ Widget memory freed from LVGL heap");
}

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
