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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// YADS-style modifier symbols using NerdFont icons
static const char *mod_symbols[4] = {
    "󰘴",  // Control (U+F0634)
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
    
    // Debug: Always log modifier flags, even if zero
    LOG_INF("🔧 MODIFIER DEBUG: Raw flags=0x%02X", mod_flags);
    
    // Collect active modifiers (YADS style - only show active ones)
    const char *active_mods[4];
    int active_count = 0;
    
    if (mod_flags & (ZMK_MOD_FLAG_LCTL | ZMK_MOD_FLAG_RCTL)) {
        active_mods[active_count++] = mod_symbols[0]; // Control
        LOG_INF("🔧 MODIFIER: Adding Control to display");
    }
    if (mod_flags & (ZMK_MOD_FLAG_LSFT | ZMK_MOD_FLAG_RSFT)) {
        active_mods[active_count++] = mod_symbols[1]; // Shift
        LOG_INF("🔧 MODIFIER: Adding Shift to display");
    }
    if (mod_flags & (ZMK_MOD_FLAG_LALT | ZMK_MOD_FLAG_RALT)) {
        active_mods[active_count++] = mod_symbols[2]; // Alt
        LOG_INF("🔧 MODIFIER: Adding Alt to display");
    }
    if (mod_flags & (ZMK_MOD_FLAG_LGUI | ZMK_MOD_FLAG_RGUI)) {
        active_mods[active_count++] = mod_symbols[3]; // GUI
        LOG_INF("🔧 MODIFIER: Adding GUI to display");
    }
    
    // Build display text (space-separated like YADS)
    for (int i = 0; i < active_count; i++) {
        if (i > 0) {
            idx += snprintf(&text[idx], sizeof(text) - idx, " ");
        }
        idx += snprintf(&text[idx], sizeof(text) - idx, "%s", active_mods[i]);
    }
    
    // Update single label with all active modifiers
    lv_label_set_text(widget->label, idx ? text : "");
    
    // Enhanced debug logging
    LOG_INF("🔧 MODIFIER: Final display - flags=0x%02X, active_count=%d, text='%s'", 
            mod_flags, active_count, text);
    
    // Debug: Force a test display if no modifiers are detected
    if (mod_flags == 0) {
        static int test_counter = 0;
        test_counter++;
        if (test_counter % 20 == 0) { // Every 20 updates (about 10 seconds at 500ms)
            LOG_INF("🔧 MODIFIER: No modifiers detected, showing test display");
            lv_label_set_text(widget->label, "TEST");
            k_sleep(K_MSEC(100));
            lv_label_set_text(widget->label, "");
        }
    }
}

int zmk_widget_modifier_status_init(struct zmk_widget_modifier_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }
    
    // Create container widget for modifier display (YADS style)
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 180, 30); // Wide enough for multiple modifiers
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    
    // Create single label for displaying active modifiers (YADS style)
    widget->label = lv_label_create(widget->obj);
    lv_obj_align(widget->label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(widget->label, ""); // Initially empty
    
    // Set font - use default font for NerdFont symbols (YADS uses custom NerdFont but we'll use default for now)
    lv_obj_set_style_text_font(widget->label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(widget->label, lv_color_white(), 0);
    
    LOG_INF("YADS-style modifier status widget initialized");
    return 0;
}

void zmk_widget_modifier_status_update(struct zmk_widget_modifier_status *widget, struct zmk_keyboard_status *kbd) {
    update_modifier_display(widget, kbd);
}

lv_obj_t *zmk_widget_modifier_status_obj(struct zmk_widget_modifier_status *widget) {
    return widget ? widget->obj : NULL;
}