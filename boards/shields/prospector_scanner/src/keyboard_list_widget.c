/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "keyboard_list_widget.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zmk/status_scanner.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

int zmk_widget_keyboard_list_init(struct zmk_widget_keyboard_list *widget, lv_obj_t *parent) {
    LOG_INF("⌨️  Keyboard list widget init START");

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
    lv_obj_set_size(widget->obj, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(widget->obj, 0, 0);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x0A0A0A), 0);  // Very dark gray
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(widget->obj, 0, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);

    // Title label at top
    LOG_INF("Creating title label...");
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "Active Keyboards");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(widget->title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 20);
    LOG_INF("✅ Title label created");

    // Create keyboard labels - 2 keyboards
    LOG_INF("Creating keyboard 1 label...");
    widget->kb1_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb1_label, "");
    lv_obj_set_style_text_color(widget->kb1_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(widget->kb1_label, &lv_font_montserrat_16, 0);
    lv_obj_align(widget->kb1_label, LV_ALIGN_TOP_LEFT, 20, 55);

    LOG_INF("Creating keyboard 2 label...");
    widget->kb2_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb2_label, "");
    lv_obj_set_style_text_color(widget->kb2_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(widget->kb2_label, &lv_font_montserrat_16, 0);
    lv_obj_align(widget->kb2_label, LV_ALIGN_TOP_LEFT, 20, 90);

    LOG_INF("Creating keyboard 3 label...");
    widget->kb3_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb3_label, "");
    lv_obj_set_style_text_color(widget->kb3_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(widget->kb3_label, &lv_font_montserrat_16, 0);
    lv_obj_align(widget->kb3_label, LV_ALIGN_TOP_LEFT, 20, 125);

    LOG_INF("✅ 3 keyboard labels created");

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ Keyboard list widget created");
    return 0;
}

// Dynamic allocation: Create widget with memory allocation
struct zmk_widget_keyboard_list *zmk_widget_keyboard_list_create(lv_obj_t *parent) {
    LOG_INF("🔷 Creating keyboard list widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("❌ Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory for widget structure using LVGL's memory allocator
    struct zmk_widget_keyboard_list *widget =
        (struct zmk_widget_keyboard_list *)lv_mem_alloc(sizeof(struct zmk_widget_keyboard_list));
    if (!widget) {
        LOG_ERR("❌ Failed to allocate memory for keyboard_list_widget (%d bytes)",
                sizeof(struct zmk_widget_keyboard_list));
        return NULL;
    }

    // Zero-initialize the allocated memory
    memset(widget, 0, sizeof(struct zmk_widget_keyboard_list));

    LOG_INF("✅ Allocated %d bytes for widget structure from LVGL heap",
            sizeof(struct zmk_widget_keyboard_list));

    // Initialize widget (this creates LVGL objects)
    int ret = zmk_widget_keyboard_list_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("❌ Widget initialization failed, freeing memory");
        lv_mem_free(widget);
        return NULL;
    }

    LOG_INF("✅ Keyboard list widget created successfully");
    return widget;
}

// Dynamic deallocation: Destroy widget and free memory
void zmk_widget_keyboard_list_destroy(struct zmk_widget_keyboard_list *widget) {
    LOG_INF("🔶 Destroying keyboard list widget (dynamic deallocation)");

    if (!widget) {
        LOG_WRN("⚠️  Widget is NULL, nothing to destroy");
        return;
    }

    // Delete LVGL objects first (in reverse order of creation)
    if (widget->kb3_label) {
        lv_obj_del(widget->kb3_label);
        widget->kb3_label = NULL;
    }
    if (widget->kb2_label) {
        lv_obj_del(widget->kb2_label);
        widget->kb2_label = NULL;
    }
    if (widget->kb1_label) {
        lv_obj_del(widget->kb1_label);
        widget->kb1_label = NULL;
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

void zmk_widget_keyboard_list_update(struct zmk_widget_keyboard_list *widget) {
    if (!widget) {
        return;
    }

    LOG_INF("Update: fetching keyboards");

    // Keyboard 1
    struct zmk_keyboard_status *kb1 = zmk_status_scanner_get_keyboard(0);
    if (kb1 && kb1->active) {
        const char *name = kb1->ble_name[0] != '\0' ? kb1->ble_name : "Unknown";
        lv_label_set_text(widget->kb1_label, name);
        LOG_INF("KB1: %s", name);
    } else {
        lv_label_set_text(widget->kb1_label, "");
    }

    // Keyboard 2
    struct zmk_keyboard_status *kb2 = zmk_status_scanner_get_keyboard(1);
    if (kb2 && kb2->active) {
        const char *name = kb2->ble_name[0] != '\0' ? kb2->ble_name : "Unknown";
        lv_label_set_text(widget->kb2_label, name);
        LOG_INF("KB2: %s", name);
    } else {
        lv_label_set_text(widget->kb2_label, "");
    }

    // Keyboard 3
    struct zmk_keyboard_status *kb3 = zmk_status_scanner_get_keyboard(2);
    if (kb3 && kb3->active) {
        const char *name = kb3->ble_name[0] != '\0' ? kb3->ble_name : "Unknown";
        lv_label_set_text(widget->kb3_label, name);
        LOG_INF("KB3: %s", name);
    } else {
        lv_label_set_text(widget->kb3_label, "");
    }

    LOG_INF("Update: done");
}

void zmk_widget_keyboard_list_show(struct zmk_widget_keyboard_list *widget) {
    LOG_INF("🔵 zmk_widget_keyboard_list_show() CALLED");

    if (!widget || !widget->obj) {
        LOG_ERR("⚠️  Widget or widget->obj is NULL, cannot show");
        return;
    }

    // Update keyboard list before showing
    zmk_widget_keyboard_list_update(widget);

    // Show the overlay
    lv_obj_move_foreground(widget->obj);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
    LOG_INF("✅ Keyboard list screen shown");
}

void zmk_widget_keyboard_list_hide(struct zmk_widget_keyboard_list *widget) {
    LOG_INF("🔴 zmk_widget_keyboard_list_hide() CALLED");

    if (widget && widget->obj) {
        // Hide the overlay
        lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
        LOG_INF("✅ Keyboard list screen hidden");
    } else {
        LOG_WRN("⚠️  Cannot hide - widget or obj is NULL");
    }
}
