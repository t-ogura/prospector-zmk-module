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

#define UPDATE_INTERVAL_MS 1000  // Update every 1 second

// ========== RSSI Helper Functions ==========

// Convert RSSI to 0-5 bar level
static uint8_t rssi_to_bars(int8_t rssi) {
    if (rssi >= -50) return 5;  // Excellent signal
    if (rssi >= -60) return 4;  // Good signal
    if (rssi >= -70) return 3;  // Fair signal
    if (rssi >= -80) return 2;  // Weak signal
    if (rssi >= -90) return 1;  // Very weak signal
    return 0;                   // No/poor signal
}

// Get color-coded RSSI bar color (battery widget color scheme)
static lv_color_t get_rssi_color(uint8_t bars) {
    if (bars >= 5) {
        return lv_color_hex(0x00CC66);  // Green (excellent)
    } else if (bars >= 4) {
        return lv_color_hex(0x66CC00);  // Light Green (good)
    } else if (bars >= 3) {
        return lv_color_hex(0xFFCC00);  // Yellow (fair)
    } else if (bars >= 2) {
        return lv_color_hex(0xFF8800);  // Orange (weak)
    } else if (bars >= 1) {
        return lv_color_hex(0xFF3333);  // Red (very weak)
    } else {
        return lv_color_hex(0x606060);  // Gray (no signal)
    }
}

// ========== Dynamic Keyboard Entry Creation ==========

// Create a single keyboard entry at specified Y position
static void create_keyboard_entry(struct zmk_widget_keyboard_list *widget, uint8_t index, int y_pos) {
    if (index >= MAX_KEYBOARD_ENTRIES) {
        return;
    }

    struct keyboard_entry *entry = &widget->entries[index];

    // RSSI bar (compact, 30px width)
    entry->rssi_bar = lv_bar_create(widget->obj);
    lv_obj_set_size(entry->rssi_bar, 30, 8);
    lv_bar_set_range(entry->rssi_bar, 0, 5);
    lv_bar_set_value(entry->rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(entry->rssi_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(entry->rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(entry->rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(entry->rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);  // CRITICAL for visibility
    lv_obj_set_style_radius(entry->rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(entry->rssi_bar, 2, LV_PART_INDICATOR);
    lv_obj_align(entry->rssi_bar, LV_ALIGN_TOP_LEFT, 10, y_pos);

    // RSSI value label (compact, montserrat_12)
    entry->rssi_label = lv_label_create(widget->obj);
    lv_label_set_text(entry->rssi_label, "--dBm");
    lv_obj_set_style_text_color(entry->rssi_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(entry->rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_align(entry->rssi_label, LV_ALIGN_TOP_LEFT, 45, y_pos - 4);

    // Keyboard name (montserrat_16, on the right)
    entry->name_label = lv_label_create(widget->obj);
    lv_label_set_text(entry->name_label, "");
    lv_obj_set_style_text_color(entry->name_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(entry->name_label, &lv_font_montserrat_16, 0);
    lv_obj_align(entry->name_label, LV_ALIGN_TOP_LEFT, 105, y_pos - 4);

    LOG_DBG("Created keyboard entry %d at Y=%d", index, y_pos);
}

// Destroy a single keyboard entry
static void destroy_keyboard_entry(struct keyboard_entry *entry) {
    if (entry->rssi_bar) {
        lv_obj_del(entry->rssi_bar);
        entry->rssi_bar = NULL;
    }
    if (entry->rssi_label) {
        lv_obj_del(entry->rssi_label);
        entry->rssi_label = NULL;
    }
    if (entry->name_label) {
        lv_obj_del(entry->name_label);
        entry->name_label = NULL;
    }
}

// Update keyboard entries based on active keyboard count
static void update_keyboard_entries(struct zmk_widget_keyboard_list *widget) {
    // Count active keyboards
    uint8_t active_count = 0;
    for (int i = 0; i < CONFIG_PROSPECTOR_MAX_KEYBOARDS; i++) {
        struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
        if (kbd && kbd->active) {
            active_count++;
        }
    }

    LOG_DBG("Active keyboards: %d (current entries: %d)", active_count, widget->entry_count);

    // If count changed, recreate entries
    if (active_count != widget->entry_count) {
        LOG_INF("Keyboard count changed: %d -> %d, recreating entries",
                widget->entry_count, active_count);

        // Destroy all existing entries
        for (int i = 0; i < widget->entry_count; i++) {
            destroy_keyboard_entry(&widget->entries[i]);
        }

        // Create new entries
        widget->entry_count = active_count > MAX_KEYBOARD_ENTRIES ?
                              MAX_KEYBOARD_ENTRIES : active_count;

        int start_y = 60;  // First keyboard Y position
        int spacing = 35;  // 35px spacing between keyboards

        for (int i = 0; i < widget->entry_count; i++) {
            create_keyboard_entry(widget, i, start_y + (i * spacing));
        }
    }

    // Update existing entries with current data
    int entry_idx = 0;
    for (int i = 0; i < CONFIG_PROSPECTOR_MAX_KEYBOARDS && entry_idx < widget->entry_count; i++) {
        struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
        if (!kbd || !kbd->active) {
            continue;
        }

        struct keyboard_entry *entry = &widget->entries[entry_idx];

        // Update keyboard name
        const char *name = kbd->ble_name[0] != '\0' ? kbd->ble_name : "Unknown";
        lv_label_set_text(entry->name_label, name);

        // Update RSSI bar and label
        uint8_t bars = rssi_to_bars(kbd->rssi);
        lv_bar_set_value(entry->rssi_bar, bars, LV_ANIM_OFF);
        lv_color_t rssi_color = get_rssi_color(bars);
        lv_obj_set_style_bg_color(entry->rssi_bar, rssi_color, LV_PART_INDICATOR);
        lv_label_set_text_fmt(entry->rssi_label, "%ddBm", kbd->rssi);

        entry_idx++;
    }
}

// ========== Periodic Update Timer ==========

static void update_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct zmk_widget_keyboard_list *widget =
        CONTAINER_OF(dwork, struct zmk_widget_keyboard_list, update_work);

    if (!widget || !widget->obj) {
        return;
    }

    // Update keyboard entries
    update_keyboard_entries(widget);

    // Schedule next update
    k_work_schedule(&widget->update_work, K_MSEC(UPDATE_INTERVAL_MS));
}

// ========== Widget Initialization ==========

int zmk_widget_keyboard_list_init(struct zmk_widget_keyboard_list *widget, lv_obj_t *parent) {
    LOG_INF("⌨️  Keyboard list widget init (dynamic generation)");

    if (!parent) {
        LOG_ERR("❌ Parent is NULL!");
        return -EINVAL;
    }

    widget->parent = parent;
    widget->entry_count = 0;

    // Initialize all entry pointers to NULL
    for (int i = 0; i < MAX_KEYBOARD_ENTRIES; i++) {
        widget->entries[i].rssi_bar = NULL;
        widget->entries[i].rssi_label = NULL;
        widget->entries[i].name_label = NULL;
    }

    // Create full-screen container
    widget->obj = lv_obj_create(parent);
    if (!widget->obj) {
        LOG_ERR("❌ Failed to create container!");
        return -ENOMEM;
    }

    lv_obj_set_size(widget->obj, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(widget->obj, 0, 0);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(widget->obj, 0, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);

    // Title label
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "Active Keyboards");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(widget->title_label, &lv_font_montserrat_20, 0);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 15);

    // Initialize periodic update timer
    k_work_init_delayable(&widget->update_work, update_work_handler);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ Keyboard list widget initialized (1s auto-update)");
    return 0;
}

// ========== Widget Control Functions ==========

void zmk_widget_keyboard_list_show(struct zmk_widget_keyboard_list *widget) {
    if (!widget || !widget->obj) {
        return;
    }

    LOG_INF("📱 Showing keyboard list widget");
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    // Perform initial update
    update_keyboard_entries(widget);

    // Start periodic updates (1 second interval)
    k_work_schedule(&widget->update_work, K_MSEC(UPDATE_INTERVAL_MS));
}

void zmk_widget_keyboard_list_hide(struct zmk_widget_keyboard_list *widget) {
    if (!widget || !widget->obj) {
        return;
    }

    LOG_INF("🚫 Hiding keyboard list widget");
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    // Stop periodic updates
    k_work_cancel_delayable(&widget->update_work);
}

void zmk_widget_keyboard_list_update(struct zmk_widget_keyboard_list *widget) {
    // Manual update (also triggered by timer)
    update_keyboard_entries(widget);
}

// ========== Dynamic Allocation Functions ==========

struct zmk_widget_keyboard_list *zmk_widget_keyboard_list_create(lv_obj_t *parent) {
    LOG_DBG("Creating keyboard list widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory using LVGL's allocator
    struct zmk_widget_keyboard_list *widget =
        (struct zmk_widget_keyboard_list *)lv_mem_alloc(sizeof(struct zmk_widget_keyboard_list));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for keyboard_list_widget (%d bytes)",
                sizeof(struct zmk_widget_keyboard_list));
        return NULL;
    }

    // Zero-initialize
    memset(widget, 0, sizeof(struct zmk_widget_keyboard_list));

    // Initialize widget
    int ret = zmk_widget_keyboard_list_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_mem_free(widget);
        return NULL;
    }

    LOG_DBG("Keyboard list widget created successfully");
    return widget;
}

void zmk_widget_keyboard_list_destroy(struct zmk_widget_keyboard_list *widget) {
    LOG_DBG("Destroying keyboard list widget (dynamic deallocation)");

    if (!widget) {
        return;
    }

    // Stop timer
    k_work_cancel_delayable(&widget->update_work);

    // Destroy all keyboard entries
    for (int i = 0; i < widget->entry_count; i++) {
        destroy_keyboard_entry(&widget->entries[i]);
    }

    // Delete container (deletes all children including title_label)
    if (widget->obj) {
        lv_obj_del(widget->obj);
        widget->obj = NULL;
    }

    // Free widget memory
    lv_mem_free(widget);
}
