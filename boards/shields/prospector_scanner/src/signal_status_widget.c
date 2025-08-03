/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/status_scanner.h>
#include "signal_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

// Convert RSSI to 0-5 bar level
static uint8_t rssi_to_bars(int8_t rssi) {
    if (rssi >= -50) return 5;  // Excellent signal
    if (rssi >= -60) return 4;  // Good signal
    if (rssi >= -70) return 3;  // Fair signal
    if (rssi >= -80) return 2;  // Weak signal
    if (rssi >= -90) return 1;  // Very weak signal
    return 0;                   // No/poor signal
}

// Get subtle gray color for RSSI bar (less distracting)
static lv_color_t get_rssi_color(uint8_t bars) {
    // All bars use subtle gray variations to avoid being distracting
    switch (bars) {
        case 5: return lv_color_make(0xC0, 0xC0, 0xC0);  // Light Gray - Excellent
        case 4: return lv_color_make(0xA0, 0xA0, 0xA0);  // Medium Light Gray - Good
        case 3: return lv_color_make(0x80, 0x80, 0x80);  // Medium Gray - Fair
        case 2: return lv_color_make(0x60, 0x60, 0x60);  // Dark Gray - Weak
        case 1: return lv_color_make(0x40, 0x40, 0x40);  // Darker Gray - Very weak
        default: return lv_color_make(0x20, 0x20, 0x20); // Very Dark Gray - Poor/No signal
    }
}

void zmk_widget_signal_status_update(struct zmk_widget_signal_status *widget, int8_t rssi) {
    if (!widget || !widget->obj) {
        return;
    }

    // Rate limiting: Update at most once per second (1Hz)
    uint32_t now = k_uptime_get_32();
    if (widget->last_update_time > 0 && (now - widget->last_update_time) < 1000) {
        return; // Skip update if less than 1 second has passed
    }

    // Calculate reception rate
    if (widget->last_update_time > 0) {
        uint32_t delta_ms = now - widget->last_update_time;
        if (delta_ms > 0) {
            widget->last_rate_hz = 1000.0f / (float)delta_ms;
        }
    }
    widget->last_update_time = now;

    // Update RSSI bar
    uint8_t bars = rssi_to_bars(rssi);
    lv_bar_set_value(widget->rssi_bar, bars, LV_ANIM_OFF);
    lv_color_t rssi_color = get_rssi_color(bars);
    lv_obj_set_style_bg_color(widget->rssi_bar, rssi_color, LV_PART_INDICATOR);

    // Update RSSI value label
    lv_label_set_text_fmt(widget->rssi_label, "%ddBm", rssi);

    // Update reception rate label with manual formatting (float formatting issues in LVGL)
    if (widget->last_rate_hz > 0) {
        char rate_text[16];
        int rate_int = (int)(widget->last_rate_hz * 10);  // Convert to tenths
        snprintf(rate_text, sizeof(rate_text), "%d.%dHz", rate_int / 10, rate_int % 10);
        lv_label_set_text(widget->rate_label, rate_text);
    } else {
        lv_label_set_text(widget->rate_label, "--Hz");
    }

    LOG_DBG("Signal status update: RSSI=%ddBm (%d bars), Rate=%.1fHz", 
            rssi, bars, widget->last_rate_hz);
}

int zmk_widget_signal_status_init(struct zmk_widget_signal_status *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }

    // Create container - much wider for proper text display
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, lv_pct(100), 25);  // Full width and taller for text
    lv_obj_set_style_bg_opa(widget->obj, 0, 0);  // Transparent background
    lv_obj_set_style_border_opa(widget->obj, 0, 0);  // No border
    lv_obj_set_style_pad_all(widget->obj, 0, 0);  // No padding
    lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(widget->obj, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);  // Right-aligned content

    // Signal info title/label (short abbreviation) - restore readable font
    lv_obj_t *signal_title = lv_label_create(widget->obj);
    lv_label_set_text(signal_title, "RX:");
    lv_obj_set_style_text_font(signal_title, &lv_font_montserrat_12, 0);  // Restore readable font
    lv_obj_set_style_text_color(signal_title, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_obj_set_width(signal_title, 30);  // Wider for readability

    // RSSI bar (small, 5-level indicator) - optimal size
    widget->rssi_bar = lv_bar_create(widget->obj);
    lv_obj_set_size(widget->rssi_bar, 30, 8);  // Restore reasonable bar width
    lv_bar_set_range(widget->rssi_bar, 0, 5);
    lv_bar_set_value(widget->rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->rssi_bar, LV_OPA_COVER, LV_PART_MAIN);  // Ensure background is visible
    lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);  // Default gray
    lv_obj_set_style_bg_opa(widget->rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);  // Ensure indicator is visible
    lv_obj_set_style_radius(widget->rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(widget->rssi_bar, 2, LV_PART_INDICATOR);

    // RSSI value label (dBm) - restore readable font with wide area
    widget->rssi_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->rssi_label, "-99dBm");  // Set reasonable width text first
    lv_obj_set_style_text_font(widget->rssi_label, &lv_font_montserrat_12, 0);  // Restore readable font
    lv_obj_set_style_text_color(widget->rssi_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_obj_set_width(widget->rssi_label, 60);  // Much wider to prevent wrapping
    lv_label_set_text(widget->rssi_label, "--dBm");  // Reset to initial text

    // Reception rate label (Hz) - restore readable font with adequate width
    widget->rate_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->rate_label, "9.9Hz");  // Set reasonable width text first (1Hz rate)
    lv_obj_set_style_text_font(widget->rate_label, &lv_font_montserrat_12, 0);  // Restore readable font
    lv_obj_set_style_text_color(widget->rate_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_obj_set_width(widget->rate_label, 50);  // Adequate width for readability
    lv_label_set_text(widget->rate_label, "--Hz");  // Reset to initial text

    // Initialize timing
    widget->last_update_time = 0;
    widget->last_rate_hz = 0;

    LOG_INF("Signal status widget initialized (RSSI + reception rate)");
    return 0;
}

void zmk_widget_signal_status_reset(struct zmk_widget_signal_status *widget) {
    if (!widget || !widget->rssi_bar || !widget->rssi_label || !widget->rate_label) {
        return;
    }
    
    LOG_INF("Signal widget reset - clearing signal status");
    
    // Reset RSSI bar to minimum
    lv_bar_set_value(widget->rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    
    // Reset RSSI label
    lv_label_set_text(widget->rssi_label, "---dBm");
    
    // Reset rate label
    lv_label_set_text(widget->rate_label, "0.0Hz");
    
    // Reset cached values
    widget->last_update_time = 0;
    widget->last_rate_hz = 0.0f;
}

lv_obj_t *zmk_widget_signal_status_obj(struct zmk_widget_signal_status *widget) {
    return widget ? widget->obj : NULL;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY