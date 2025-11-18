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

// ========== RSSI Helper Functions (from signal_status_widget.c) ==========

// Convert RSSI to 0-5 bar level
static uint8_t rssi_to_bars(int8_t rssi) {
    if (rssi >= -50) return 5;  // Excellent signal
    if (rssi >= -60) return 4;  // Good signal
    if (rssi >= -70) return 3;  // Fair signal
    if (rssi >= -80) return 2;  // Weak signal
    if (rssi >= -90) return 1;  // Very weak signal
    return 0;                   // No/poor signal
}

// Get color-coded RSSI bar color (same as battery widget - 5 levels)
static lv_color_t get_rssi_color(uint8_t bars) {
    // 5-level color scheme matching battery widget
    if (bars >= 5) {
        // 80%+ Green (5 bars = excellent signal)
        return lv_color_hex(0x00CC66);
    } else if (bars >= 4) {
        // 60-79% Light Green (4 bars = good signal)
        return lv_color_hex(0x66CC00);
    } else if (bars >= 3) {
        // 40-59% Yellow (3 bars = fair signal)
        return lv_color_hex(0xFFCC00);
    } else if (bars >= 2) {
        // 20-39% Orange (2 bars = weak signal)
        return lv_color_hex(0xFF8800);
    } else if (bars >= 1) {
        // <20% Red (1 bar = very weak signal)
        return lv_color_hex(0xFF3333);
    } else {
        // 0 bars = no signal (gray)
        return lv_color_hex(0x606060);
    }
}

// ========== Widget Initialization ==========

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
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 15);
    LOG_INF("✅ Title label created");

    // ========== Keyboard 1: RSSI Bar + Value + Name (horizontal layout) ==========
    LOG_INF("Creating keyboard 1 elements...");

    // RSSI bar (compact, color-coded)
    widget->kb1_rssi_bar = lv_bar_create(widget->obj);
    lv_obj_set_size(widget->kb1_rssi_bar, 30, 8);  // Much smaller
    lv_bar_set_range(widget->kb1_rssi_bar, 0, 5);
    lv_bar_set_value(widget->kb1_rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widget->kb1_rssi_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->kb1_rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->kb1_rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(widget->kb1_rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);  // IMPORTANT: Make indicator visible
    lv_obj_set_style_radius(widget->kb1_rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->kb1_rssi_bar, 2, LV_PART_INDICATOR);
    lv_obj_align(widget->kb1_rssi_bar, LV_ALIGN_TOP_LEFT, 10, 60);

    // RSSI value label (compact)
    widget->kb1_rssi_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb1_rssi_label, "--dBm");
    lv_obj_set_style_text_color(widget->kb1_rssi_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(widget->kb1_rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_align(widget->kb1_rssi_label, LV_ALIGN_TOP_LEFT, 45, 56);

    // Keyboard name (on the right)
    widget->kb1_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb1_label, "");
    lv_obj_set_style_text_color(widget->kb1_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(widget->kb1_label, &lv_font_montserrat_16, 0);
    lv_obj_align(widget->kb1_label, LV_ALIGN_TOP_LEFT, 105, 56);

    // ========== Keyboard 2: RSSI Bar + Value + Name (horizontal layout) ==========
    LOG_INF("Creating keyboard 2 elements...");

    // RSSI bar (compact)
    widget->kb2_rssi_bar = lv_bar_create(widget->obj);
    lv_obj_set_size(widget->kb2_rssi_bar, 30, 8);
    lv_bar_set_range(widget->kb2_rssi_bar, 0, 5);
    lv_bar_set_value(widget->kb2_rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widget->kb2_rssi_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->kb2_rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->kb2_rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(widget->kb2_rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);  // IMPORTANT: Make indicator visible
    lv_obj_set_style_radius(widget->kb2_rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->kb2_rssi_bar, 2, LV_PART_INDICATOR);
    lv_obj_align(widget->kb2_rssi_bar, LV_ALIGN_TOP_LEFT, 10, 95);

    // RSSI value label (compact)
    widget->kb2_rssi_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb2_rssi_label, "--dBm");
    lv_obj_set_style_text_color(widget->kb2_rssi_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(widget->kb2_rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_align(widget->kb2_rssi_label, LV_ALIGN_TOP_LEFT, 45, 91);

    // Keyboard name (on the right)
    widget->kb2_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb2_label, "");
    lv_obj_set_style_text_color(widget->kb2_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(widget->kb2_label, &lv_font_montserrat_16, 0);
    lv_obj_align(widget->kb2_label, LV_ALIGN_TOP_LEFT, 105, 91);

    // ========== Keyboard 3: RSSI Bar + Value + Name (horizontal layout) ==========
    LOG_INF("Creating keyboard 3 elements...");

    // RSSI bar (compact)
    widget->kb3_rssi_bar = lv_bar_create(widget->obj);
    lv_obj_set_size(widget->kb3_rssi_bar, 30, 8);
    lv_bar_set_range(widget->kb3_rssi_bar, 0, 5);
    lv_bar_set_value(widget->kb3_rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widget->kb3_rssi_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->kb3_rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->kb3_rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(widget->kb3_rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);  // IMPORTANT: Make indicator visible
    lv_obj_set_style_radius(widget->kb3_rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->kb3_rssi_bar, 2, LV_PART_INDICATOR);
    lv_obj_align(widget->kb3_rssi_bar, LV_ALIGN_TOP_LEFT, 10, 130);

    // RSSI value label (compact)
    widget->kb3_rssi_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb3_rssi_label, "--dBm");
    lv_obj_set_style_text_color(widget->kb3_rssi_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(widget->kb3_rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_align(widget->kb3_rssi_label, LV_ALIGN_TOP_LEFT, 45, 126);

    // Keyboard name (on the right)
    widget->kb3_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->kb3_label, "");
    lv_obj_set_style_text_color(widget->kb3_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(widget->kb3_label, &lv_font_montserrat_16, 0);
    lv_obj_align(widget->kb3_label, LV_ALIGN_TOP_LEFT, 105, 126);

    LOG_INF("✅ 3 keyboards with compact RSSI displays created");

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

    // Delete LVGL objects (parent obj will delete all children automatically)
    if (widget->obj) {
        lv_obj_del(widget->obj);  // This deletes all child objects too
        widget->obj = NULL;
        // Clear all child pointers for safety
        widget->title_label = NULL;
        widget->kb1_label = NULL;
        widget->kb1_rssi_bar = NULL;
        widget->kb1_rssi_label = NULL;
        widget->kb2_label = NULL;
        widget->kb2_rssi_bar = NULL;
        widget->kb2_rssi_label = NULL;
        widget->kb3_label = NULL;
        widget->kb3_rssi_bar = NULL;
        widget->kb3_rssi_label = NULL;
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

    LOG_INF("Update: fetching keyboards with RSSI");

    // ========== Keyboard 1 ==========
    struct zmk_keyboard_status *kb1 = zmk_status_scanner_get_keyboard(0);
    if (kb1 && kb1->active) {
        const char *name = kb1->ble_name[0] != '\0' ? kb1->ble_name : "Unknown";
        lv_label_set_text(widget->kb1_label, name);

        // Update RSSI bar and label
        uint8_t bars = rssi_to_bars(kb1->rssi);
        lv_bar_set_value(widget->kb1_rssi_bar, bars, LV_ANIM_OFF);
        lv_color_t rssi_color = get_rssi_color(bars);
        lv_obj_set_style_bg_color(widget->kb1_rssi_bar, rssi_color, LV_PART_INDICATOR);
        lv_label_set_text_fmt(widget->kb1_rssi_label, "%ddBm", kb1->rssi);

        LOG_INF("KB1: %s (%ddBm, %d bars)", name, kb1->rssi, bars);
    } else {
        lv_label_set_text(widget->kb1_label, "");
        lv_bar_set_value(widget->kb1_rssi_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(widget->kb1_rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
        lv_label_set_text(widget->kb1_rssi_label, "--dBm");
    }

    // ========== Keyboard 2 ==========
    struct zmk_keyboard_status *kb2 = zmk_status_scanner_get_keyboard(1);
    if (kb2 && kb2->active) {
        const char *name = kb2->ble_name[0] != '\0' ? kb2->ble_name : "Unknown";
        lv_label_set_text(widget->kb2_label, name);

        // Update RSSI bar and label
        uint8_t bars = rssi_to_bars(kb2->rssi);
        lv_bar_set_value(widget->kb2_rssi_bar, bars, LV_ANIM_OFF);
        lv_color_t rssi_color = get_rssi_color(bars);
        lv_obj_set_style_bg_color(widget->kb2_rssi_bar, rssi_color, LV_PART_INDICATOR);
        lv_label_set_text_fmt(widget->kb2_rssi_label, "%ddBm", kb2->rssi);

        LOG_INF("KB2: %s (%ddBm, %d bars)", name, kb2->rssi, bars);
    } else {
        lv_label_set_text(widget->kb2_label, "");
        lv_bar_set_value(widget->kb2_rssi_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(widget->kb2_rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
        lv_label_set_text(widget->kb2_rssi_label, "--dBm");
    }

    // ========== Keyboard 3 ==========
    struct zmk_keyboard_status *kb3 = zmk_status_scanner_get_keyboard(2);
    if (kb3 && kb3->active) {
        const char *name = kb3->ble_name[0] != '\0' ? kb3->ble_name : "Unknown";
        lv_label_set_text(widget->kb3_label, name);

        // Update RSSI bar and label
        uint8_t bars = rssi_to_bars(kb3->rssi);
        lv_bar_set_value(widget->kb3_rssi_bar, bars, LV_ANIM_OFF);
        lv_color_t rssi_color = get_rssi_color(bars);
        lv_obj_set_style_bg_color(widget->kb3_rssi_bar, rssi_color, LV_PART_INDICATOR);
        lv_label_set_text_fmt(widget->kb3_rssi_label, "%ddBm", kb3->rssi);

        LOG_INF("KB3: %s (%ddBm, %d bars)", name, kb3->rssi, bars);
    } else {
        lv_label_set_text(widget->kb3_label, "");
        lv_bar_set_value(widget->kb3_rssi_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(widget->kb3_rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
        lv_label_set_text(widget->kb3_rssi_label, "--dBm");
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
