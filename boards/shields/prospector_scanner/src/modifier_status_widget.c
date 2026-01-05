/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/logging/log.h>
#include <zmk/status_advertisement.h>
#include <zmk/status_scanner.h>
#include "modifier_status_widget.h"
#include "fonts.h"  // NerdFont declarations

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Modifier symbols - using YADS NerdFont icons (MIT License)
static const char *mod_symbols[4] = {
    "󰘴",  // Control (U+F0634) - exactly like YADS
    "󰘶",  // Shift (U+F0636)
    "󰘵",  // Alt (U+F0635)  
    "󰘳"   // GUI/Win/Cmd (U+F0633)
};

static void update_modifier_display(struct zmk_widget_modifier_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !kbd) {
        LOG_WRN("MODIFIER: Widget or keyboard data is NULL");
        return;
    }
    
    uint8_t mod_flags = kbd->data.modifier_flags;
    char text[64] = "";
    int idx = 0;
    
    // Removed debug logging for performance
    
    // Collect active modifiers (YADS style - only show active ones)
    const char *active_mods[4];
    int active_count = 0;
    
    if (mod_flags & (ZMK_MOD_FLAG_LCTL | ZMK_MOD_FLAG_RCTL)) {
        active_mods[active_count++] = mod_symbols[0]; // Control
    }
    if (mod_flags & (ZMK_MOD_FLAG_LSFT | ZMK_MOD_FLAG_RSFT)) {
        active_mods[active_count++] = mod_symbols[1]; // Shift
    }
    if (mod_flags & (ZMK_MOD_FLAG_LALT | ZMK_MOD_FLAG_RALT)) {
        active_mods[active_count++] = mod_symbols[2]; // Alt
    }
    if (mod_flags & (ZMK_MOD_FLAG_LGUI | ZMK_MOD_FLAG_RGUI)) {
        active_mods[active_count++] = mod_symbols[3]; // GUI
    }
    
    // Build display text without spaces - use letter spacing instead
    for (int i = 0; i < active_count; i++) {
        idx += snprintf(&text[idx], sizeof(text) - idx, "%s", active_mods[i]);
    }
    
    // Update single label with all active modifiers
    lv_label_set_text(widget->label, idx ? text : "");
    
    // Debug logging removed for performance
}

/**
 * LVGL 9 FIX: NO CONTAINER - Create all elements directly on parent
 * Widget positioned at CENTER with y=30 in scanner_display.c
 */
int zmk_widget_modifier_status_init(struct zmk_widget_modifier_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }

    widget->parent = parent;

    // Create single label for displaying active modifiers - directly on parent
    widget->label = lv_label_create(parent);
    lv_obj_align(widget->label, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(widget->label, "");

    // Set font - use 40px NerdFont
    lv_obj_set_style_text_font(widget->label, &NerdFonts_Regular_40, 0);
    lv_obj_set_style_text_color(widget->label, lv_color_white(), 0);

    // Set letter spacing to separate modifier symbols
    lv_obj_set_style_text_letter_space(widget->label, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Set widget->obj to label for compatibility
    widget->obj = widget->label;

    LOG_INF("✨ Modifier status widget initialized (LVGL9 no-container pattern)");
    return 0;
}

// Dynamic allocation: Create widget with memory allocation
struct zmk_widget_modifier_status *zmk_widget_modifier_status_create(lv_obj_t *parent) {
    LOG_DBG("Creating modifier status widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory for widget structure using LVGL's memory allocator
    struct zmk_widget_modifier_status *widget =
        (struct zmk_widget_modifier_status *)lv_malloc(sizeof(struct zmk_widget_modifier_status));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for modifier_status_widget (%d bytes)",
                sizeof(struct zmk_widget_modifier_status));
        return NULL;
    }

    // Zero-initialize the allocated memory
    memset(widget, 0, sizeof(struct zmk_widget_modifier_status));

    // Initialize widget (this creates LVGL objects)
    int ret = zmk_widget_modifier_status_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_free(widget);
        return NULL;
    }

    LOG_DBG("Modifier status widget created successfully");
    return widget;
}

// Dynamic deallocation: Destroy widget and free memory
void zmk_widget_modifier_status_destroy(struct zmk_widget_modifier_status *widget) {
    LOG_DBG("Destroying modifier status widget (LVGL9 no-container)");

    if (!widget) {
        return;
    }

    // LVGL 9 FIX: Delete each element individually (no container parent)
    // Note: widget->obj and widget->label point to the same object now
    if (widget->label) {
        lv_obj_del(widget->label);
        widget->label = NULL;
    }

    widget->obj = NULL;
    widget->parent = NULL;

    // Free the widget structure memory from LVGL heap
    lv_free(widget);
}

void zmk_widget_modifier_status_update(struct zmk_widget_modifier_status *widget, struct zmk_keyboard_status *kbd) {
    update_modifier_display(widget, kbd);
}

void zmk_widget_modifier_status_reset(struct zmk_widget_modifier_status *widget) {
    if (!widget || !widget->label) {
        return;
    }
    
    LOG_INF("Modifier widget reset - clearing all modifier displays");
    
    // Clear modifier display - empty text means no modifiers active
    lv_label_set_text(widget->label, "");
}

lv_obj_t *zmk_widget_modifier_status_obj(struct zmk_widget_modifier_status *widget) {
    return widget ? widget->obj : NULL;
}