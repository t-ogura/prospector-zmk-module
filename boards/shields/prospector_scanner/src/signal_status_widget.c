/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
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

// Get color for RSSI bar based on strength
static lv_color_t get_rssi_color(uint8_t bars) {
    switch (bars) {
        case 5: return lv_color_make(0x00, 0xFF, 0x00);  // Green - Excellent
        case 4: return lv_color_make(0x7F, 0xFF, 0x00);  // Light Green - Good
        case 3: return lv_color_make(0xFF, 0xFF, 0x00);  // Yellow - Fair
        case 2: return lv_color_make(0xFF, 0x7F, 0x00);  // Orange - Weak
        case 1: return lv_color_make(0xFF, 0x3F, 0x00);  // Red Orange - Very weak
        default: return lv_color_make(0xFF, 0x00, 0x00); // Red - Poor/No signal
    }
}

void zmk_widget_signal_status_update(struct zmk_widget_signal_status *widget, int8_t rssi) {
    if (!widget || !widget->obj) {
        return;
    }

    // Calculate reception rate
    uint32_t now = k_uptime_get_32();
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

    // Update reception rate label  
    if (widget->last_rate_hz > 0) {
        lv_label_set_text_fmt(widget->rate_label, "%.1fHz", widget->last_rate_hz);
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

    // Create container (20px height as requested)
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, lv_pct(90), 20);
    lv_obj_set_style_bg_opa(widget->obj, 0, 0);  // Transparent background
    lv_obj_set_style_border_opa(widget->obj, 0, 0);  // No border
    lv_obj_set_style_pad_all(widget->obj, 0, 0);  // No padding
    lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(widget->obj, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // RSSI bar (small, 5-level indicator)
    widget->rssi_bar = lv_bar_create(widget->obj);
    lv_obj_set_size(widget->rssi_bar, 40, 8);
    lv_bar_set_range(widget->rssi_bar, 0, 5);
    lv_bar_set_value(widget->rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0x30, 0x30, 0x30), LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0xFF, 0x00, 0x00), LV_PART_INDICATOR);
    lv_obj_set_style_radius(widget->rssi_bar, 2, LV_PART_MAIN);

    // RSSI value label (dBm)
    widget->rssi_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->rssi_label, "--dBm");
    lv_obj_set_style_text_font(widget->rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widget->rssi_label, lv_color_make(0xC0, 0xC0, 0xC0), 0);

    // Reception rate label (Hz)
    widget->rate_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->rate_label, "--Hz");
    lv_obj_set_style_text_font(widget->rate_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widget->rate_label, lv_color_make(0xC0, 0xC0, 0xC0), 0);

    // Initialize timing
    widget->last_update_time = 0;
    widget->last_rate_hz = 0;

    LOG_INF("Signal status widget initialized (RSSI + reception rate)");
    return 0;
}

lv_obj_t *zmk_widget_signal_status_obj(struct zmk_widget_signal_status *widget) {
    return widget ? widget->obj : NULL;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY