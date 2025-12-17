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

// External functions from scanner_display.c for keyboard selection
extern int zmk_scanner_get_selected_keyboard(void);
extern void zmk_scanner_set_selected_keyboard(int index);

// External flag from scanner_display.c for deadlock prevention
extern volatile bool swipe_in_progress;

// Phase 3: No longer using external mutex - LVGL timer handles thread safety

// Forward declaration for widget pointer (needed in event handler)
static struct zmk_widget_keyboard_list *g_keyboard_list_widget = NULL;

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

// Forward declaration
static void update_keyboard_entries(struct zmk_widget_keyboard_list *widget);

// Click event handler for keyboard entry selection
static void keyboard_entry_click_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        // Get keyboard index from user data
        int keyboard_index = (int)(intptr_t)lv_event_get_user_data(e);

        LOG_INF("🎯 Keyboard entry clicked: index=%d", keyboard_index);

        // Set selected keyboard
        zmk_scanner_set_selected_keyboard(keyboard_index);

        // Update visual state immediately
        // Note: Click handlers run from LVGL timer context (main thread)
        // No mutex needed here - we're already in the LVGL context
        // Mutex is only needed for work queue thread operations
        if (g_keyboard_list_widget) {
            update_keyboard_entries(g_keyboard_list_widget);
        }
    }
}

// Create a single keyboard entry at specified Y position
static void create_keyboard_entry(struct zmk_widget_keyboard_list *widget, uint8_t index, int y_pos, int keyboard_index) {
    if (index >= MAX_KEYBOARD_ENTRIES) {
        return;
    }

    struct keyboard_entry *entry = &widget->entries[index];
    entry->keyboard_index = keyboard_index;

    // Create clickable container for the entire entry
    entry->container = lv_obj_create(widget->obj);
    lv_obj_set_size(entry->container, 220, 30);
    lv_obj_align(entry->container, LV_ALIGN_TOP_MID, 0, y_pos - 8);
    lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x1A1A1A), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(entry->container, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(entry->container, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(entry->container, lv_color_hex(0x303030), LV_STATE_DEFAULT);
    lv_obj_set_style_radius(entry->container, 6, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(entry->container, 0, LV_STATE_DEFAULT);
    lv_obj_add_flag(entry->container, LV_OBJ_FLAG_CLICKABLE);

    // Register click event with keyboard index as user data
    lv_obj_add_event_cb(entry->container, keyboard_entry_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)keyboard_index);

    // RSSI bar (compact, 30px width) - child of container
    entry->rssi_bar = lv_bar_create(entry->container);
    lv_obj_set_size(entry->rssi_bar, 30, 8);
    lv_bar_set_range(entry->rssi_bar, 0, 5);
    lv_bar_set_value(entry->rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(entry->rssi_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(entry->rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(entry->rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(entry->rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(entry->rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(entry->rssi_bar, 2, LV_PART_INDICATOR);
    lv_obj_align(entry->rssi_bar, LV_ALIGN_LEFT_MID, 8, 0);

    // RSSI value label (compact, montserrat_12) - child of container
    entry->rssi_label = lv_label_create(entry->container);
    lv_label_set_text(entry->rssi_label, "--dBm");
    lv_obj_set_style_text_color(entry->rssi_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(entry->rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_align(entry->rssi_label, LV_ALIGN_LEFT_MID, 42, 0);

    // Keyboard name (montserrat_16, on the right) - child of container
    entry->name_label = lv_label_create(entry->container);
    lv_label_set_text(entry->name_label, "");
    lv_obj_set_style_text_color(entry->name_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(entry->name_label, &lv_font_montserrat_16, 0);
    lv_obj_align(entry->name_label, LV_ALIGN_LEFT_MID, 100, 0);

    LOG_DBG("Created keyboard entry %d at Y=%d (keyboard_index=%d)", index, y_pos, keyboard_index);
}

// Destroy a single keyboard entry
static void destroy_keyboard_entry(struct keyboard_entry *entry) {
    // Deleting container deletes all children (rssi_bar, rssi_label, name_label)
    if (entry->container) {
        lv_obj_del(entry->container);
        entry->container = NULL;
    }
    entry->rssi_bar = NULL;
    entry->rssi_label = NULL;
    entry->name_label = NULL;
    entry->keyboard_index = -1;
}

// Update keyboard entries based on active keyboard count
static void update_keyboard_entries(struct zmk_widget_keyboard_list *widget) {
    // Count active keyboards and collect their indices
    uint8_t active_count = 0;
    int keyboard_indices[MAX_KEYBOARD_ENTRIES] = {-1};
    for (int i = 0; i < CONFIG_PROSPECTOR_MAX_KEYBOARDS; i++) {
        struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
        if (kbd && kbd->active && active_count < MAX_KEYBOARD_ENTRIES) {
            keyboard_indices[active_count] = i;
            active_count++;
        }
    }

    LOG_DBG("Active keyboards: %d (current entries: %d)", active_count, widget->entry_count);

    // Get current selection
    int selected = zmk_scanner_get_selected_keyboard();

    // Auto-select first keyboard if none selected or selection became inactive
    if (selected == -1 && active_count > 0) {
        selected = keyboard_indices[0];
        zmk_scanner_set_selected_keyboard(selected);
    } else if (selected >= 0 && active_count > 0) {
        // Verify selected keyboard is still active
        bool found = false;
        for (int i = 0; i < active_count; i++) {
            if (keyboard_indices[i] == selected) {
                found = true;
                break;
            }
        }
        if (!found) {
            // Selected keyboard became inactive, switch to first active
            selected = keyboard_indices[0];
            zmk_scanner_set_selected_keyboard(selected);
            LOG_INF("Selected keyboard inactive, switching to index %d", selected);
        }
    }

    // If count changed, recreate entries
    if (active_count != widget->entry_count) {
        LOG_INF("Keyboard count changed: %d -> %d, recreating entries",
                widget->entry_count, active_count);

        // Destroy all existing entries
        for (int i = 0; i < widget->entry_count; i++) {
            destroy_keyboard_entry(&widget->entries[i]);
        }

        // Create new entries
        widget->entry_count = active_count;

        int start_y = 60;  // First keyboard Y position
        int spacing = 38;  // Increased spacing for larger containers

        for (int i = 0; i < widget->entry_count; i++) {
            create_keyboard_entry(widget, i, start_y + (i * spacing), keyboard_indices[i]);
        }
    }

    // Update existing entries with current data and selection state
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

        // Apply selection styling
        bool is_selected = (entry->keyboard_index == selected);
        if (is_selected) {
            // Selected: blue highlight background
            lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x2A4A6A), LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(entry->container, lv_color_hex(0x4A90E2), LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(entry->container, 2, LV_STATE_DEFAULT);
        } else {
            // Unselected: dark background
            lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x1A1A1A), LV_STATE_DEFAULT);
            lv_obj_set_style_border_color(entry->container, lv_color_hex(0x303030), LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(entry->container, 1, LV_STATE_DEFAULT);
        }

        entry_idx++;
    }
}

// ========== Phase 3: LVGL Timer for Periodic Updates ==========
// This replaces the k_work_delayable approach for thread-safe LVGL operations

static void update_timer_cb(lv_timer_t *timer) {
    struct zmk_widget_keyboard_list *widget = (struct zmk_widget_keyboard_list *)timer->user_data;

    if (!widget || !widget->obj) {
        return;
    }

    // Skip update if swipe is in progress
    if (swipe_in_progress) {
        LOG_DBG("Keyboard list update skipped - swipe in progress");
        return;
    }

    // Phase 3: No mutex needed - we're already in LVGL main thread
    // Update keyboard entries directly
    update_keyboard_entries(widget);
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
        widget->entries[i].container = NULL;
        widget->entries[i].rssi_bar = NULL;
        widget->entries[i].rssi_label = NULL;
        widget->entries[i].name_label = NULL;
        widget->entries[i].keyboard_index = -1;
    }

    // Set global widget pointer for event handler access
    g_keyboard_list_widget = widget;

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

    // Phase 3: Create LVGL timer for periodic updates (runs in LVGL main thread)
    // Timer is initially paused - will be started when widget is shown
    widget->update_timer = lv_timer_create(update_timer_cb, UPDATE_INTERVAL_MS, widget);
    if (widget->update_timer) {
        lv_timer_pause(widget->update_timer);  // Start paused
    }

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ Keyboard list widget initialized (LVGL timer, %dms interval)", UPDATE_INTERVAL_MS);
    return 0;
}

// ========== Widget Control Functions ==========

void zmk_widget_keyboard_list_show(struct zmk_widget_keyboard_list *widget) {
    if (!widget || !widget->obj) {
        return;
    }

    LOG_INF("📱 Showing keyboard list widget");
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    // Phase 3: Resume LVGL timer for periodic updates
    // Timer runs in LVGL main thread - no mutex needed
    if (widget->update_timer) {
        lv_timer_resume(widget->update_timer);
        // Trigger immediate first update
        lv_timer_ready(widget->update_timer);
    }
}

void zmk_widget_keyboard_list_hide(struct zmk_widget_keyboard_list *widget) {
    if (!widget || !widget->obj) {
        return;
    }

    LOG_INF("🚫 Hiding keyboard list widget");
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    // Phase 3: Pause LVGL timer
    if (widget->update_timer) {
        lv_timer_pause(widget->update_timer);
    }
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
        (struct zmk_widget_keyboard_list *)lv_malloc(sizeof(struct zmk_widget_keyboard_list));
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
        lv_free(widget);
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

    // Clear global widget pointer first to prevent any callbacks
    if (g_keyboard_list_widget == widget) {
        g_keyboard_list_widget = NULL;
    }

    // Phase 3: Delete LVGL timer
    if (widget->update_timer) {
        lv_timer_del(widget->update_timer);
        widget->update_timer = NULL;
    }

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
    lv_free(widget);
}
