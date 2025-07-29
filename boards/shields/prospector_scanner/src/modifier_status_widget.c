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

// Simple modifier indicator characters (no NerdFont required)
static const char *mod_chars[4] = {
    "C",  // Control
    "S",  // Shift  
    "A",  // Alt
    "G"   // GUI/Win/Cmd
};

static void update_modifier_display(struct zmk_widget_modifier_status *widget, struct zmk_keyboard_status *kbd) {
    if (!widget || !kbd) {
        return;
    }
    
    uint8_t mod_flags = kbd->data.modifier_flags;
    
    // Check each modifier and update visibility/color
    for (int i = 0; i < 4; i++) {
        bool left_active = false;
        bool right_active = false;
        
        switch (i) {
            case 0: // Control
                left_active = mod_flags & ZMK_MOD_FLAG_LCTL;
                right_active = mod_flags & ZMK_MOD_FLAG_RCTL;
                break;
            case 1: // Shift
                left_active = mod_flags & ZMK_MOD_FLAG_LSFT;
                right_active = mod_flags & ZMK_MOD_FLAG_RSFT;
                break;
            case 2: // Alt
                left_active = mod_flags & ZMK_MOD_FLAG_LALT;
                right_active = mod_flags & ZMK_MOD_FLAG_RALT;
                break;
            case 3: // GUI
                left_active = mod_flags & ZMK_MOD_FLAG_LGUI;
                right_active = mod_flags & ZMK_MOD_FLAG_RGUI;
                break;
        }
        
        if (left_active || right_active) {
            // Modifier is active - show with color
            lv_obj_clear_flag(widget->mod_labels[i], LV_OBJ_FLAG_HIDDEN);
            
            if (left_active && right_active) {
                // Both sides active - bright white
                lv_obj_set_style_text_color(widget->mod_labels[i], lv_color_white(), 0);
                lv_obj_set_style_text_opa(widget->mod_labels[i], LV_OPA_COVER, 0);
            } else if (left_active) {
                // Left only - cyan
                lv_obj_set_style_text_color(widget->mod_labels[i], lv_color_make(0, 255, 255), 0);
                lv_obj_set_style_text_opa(widget->mod_labels[i], LV_OPA_COVER, 0);
            } else {
                // Right only - magenta
                lv_obj_set_style_text_color(widget->mod_labels[i], lv_color_make(255, 0, 255), 0);
                lv_obj_set_style_text_opa(widget->mod_labels[i], LV_OPA_COVER, 0);
            }
        } else {
            // Modifier inactive - hide or show dimmed
            lv_obj_set_style_text_color(widget->mod_labels[i], lv_color_make(80, 80, 80), 0);
            lv_obj_set_style_text_opa(widget->mod_labels[i], LV_OPA_30, 0);
        }
    }
    
    LOG_DBG("Modifier status updated: flags=0x%02X", mod_flags);
}

int zmk_widget_modifier_status_init(struct zmk_widget_modifier_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }
    
    // Create container widget for modifier display
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 120, 30); // Compact horizontal layout
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    
    // Create modifier labels (C S A G) in horizontal layout
    for (int i = 0; i < 4; i++) {
        widget->mod_labels[i] = lv_label_create(widget->obj);
        
        // Set font - using existing montserrat fonts
        lv_obj_set_style_text_font(widget->mod_labels[i], &lv_font_montserrat_16, 0);
        
        // Set modifier character
        lv_label_set_text(widget->mod_labels[i], mod_chars[i]);
        
        // Position horizontally with spacing
        int spacing = 28; // Space between modifiers
        int start_x = -42; // Start position to center the row
        lv_obj_align(widget->mod_labels[i], LV_ALIGN_CENTER, start_x + (i * spacing), 0);
        
        // Initially all modifiers are dimmed
        lv_obj_set_style_text_color(widget->mod_labels[i], lv_color_make(80, 80, 80), 0);
        lv_obj_set_style_text_opa(widget->mod_labels[i], LV_OPA_30, 0);
    }
    
    LOG_INF("Modifier status widget initialized");
    return 0;
}

void zmk_widget_modifier_status_update(struct zmk_widget_modifier_status *widget, struct zmk_keyboard_status *kbd) {
    update_modifier_display(widget, kbd);
}

lv_obj_t *zmk_widget_modifier_status_obj(struct zmk_widget_modifier_status *widget) {
    return widget ? widget->obj : NULL;
}