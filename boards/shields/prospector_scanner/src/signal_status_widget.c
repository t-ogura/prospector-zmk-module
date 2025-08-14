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

// Timeout for clearing signal display when no reception (30 seconds)
#define SIGNAL_TIMEOUT_MS 30000

// Add smoothed RSSI calculation using moving average
static int8_t calculate_smoothed_rssi(struct zmk_widget_signal_status *widget, int8_t new_rssi) {
    // Add new sample to circular buffer
    widget->rssi_samples[widget->rssi_sample_index] = new_rssi;
    widget->rssi_sample_index = (widget->rssi_sample_index + 1) % RSSI_SMOOTHING_SAMPLES;
    
    // Track how many samples we have (up to buffer size)
    if (widget->rssi_sample_count < RSSI_SMOOTHING_SAMPLES) {
        widget->rssi_sample_count++;
    }
    
    // Calculate average of available samples
    int32_t sum = 0;
    for (uint8_t i = 0; i < widget->rssi_sample_count; i++) {
        sum += widget->rssi_samples[i];
    }
    
    widget->rssi_smoothed = sum / widget->rssi_sample_count;
    return widget->rssi_smoothed;
}

// Add smoothed Hz rate calculation using moving average
static float calculate_smoothed_rate(struct zmk_widget_signal_status *widget, float new_rate) {
    // Add new sample to circular buffer
    widget->rate_samples[widget->rate_sample_index] = new_rate;
    widget->rate_sample_index = (widget->rate_sample_index + 1) % RATE_SMOOTHING_SAMPLES;
    
    // Track how many samples we have (up to buffer size)
    if (widget->rate_sample_count < RATE_SMOOTHING_SAMPLES) {
        widget->rate_sample_count++;
    }
    
    // Calculate average of available samples
    float sum = 0.0f;
    for (uint8_t i = 0; i < widget->rate_sample_count; i++) {
        sum += widget->rate_samples[i];
    }
    
    widget->rate_smoothed = sum / widget->rate_sample_count;
    
    // Debug logging for rate smoothing
    LOG_INF("Rate smooth: new=%.1f, avg=%.1f, samples=%d", new_rate, widget->rate_smoothed, widget->rate_sample_count);
    
    return widget->rate_smoothed;
}

// Check for signal timeout and clear display if needed
static void check_signal_timeout(struct zmk_widget_signal_status *widget) {
    uint32_t now = k_uptime_get_32();
    
    if (widget->signal_active && 
        widget->last_signal_time > 0 && 
        (now - widget->last_signal_time) > SIGNAL_TIMEOUT_MS) {
        
        LOG_INF("Signal timeout - clearing RX display after %dms", now - widget->last_signal_time);
        
        // Clear RSSI display
        lv_bar_set_value(widget->rssi_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
        lv_label_set_text(widget->rssi_label, "--dBm");
        
        // Show decreasing rate (0.0Hz indicates no reception)
        lv_label_set_text(widget->rate_label, "0.0Hz");
        
        // Reset state
        widget->signal_active = false;
        widget->last_rate_hz = 0.0f;
        widget->reception_count = 0;
        widget->rssi_sample_count = 0;  // Clear RSSI smoothing buffer
        widget->rssi_sample_index = 0;
        widget->rate_sample_count = 0;  // Clear rate smoothing buffer
        widget->rate_sample_index = 0;
    }
}

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

    uint32_t now = k_uptime_get_32();
    
    // Record signal reception time for timeout detection
    widget->last_signal_time = now;
    widget->signal_active = true;
    
    // Calculate smoothed RSSI using moving average
    int8_t smoothed_rssi = calculate_smoothed_rssi(widget, rssi);
    
    // Track reception for rate calculation (only count Prospector data receptions)
    widget->reception_count++;
    
    // Track last update time for rate calculation
    widget->last_update_time = now;
    
    // Remove rate limiting - allow immediate display updates for responsive UI
    // The periodic 1Hz system will handle regular updates, but reception events should update immediately
    
    // Only calculate rate if enough time has passed for meaningful measurement
    // This prevents erratic rate calculations from individual receptions
    float current_rate_hz = widget->last_rate_hz;  // Keep current rate by default
    
    if (widget->last_display_update > 0) {
        uint32_t interval_ms = now - widget->last_display_update;
        
        // Update rate calculation with 3-second buffer for balance
        // Display updates 1Hz but uses 10-sample moving average for stability
        if (interval_ms >= 3000) {
            if (widget->reception_count > 0) {
                // Calculate average rate: receptions per second
                current_rate_hz = (widget->reception_count * 1000.0f) / interval_ms;
                
                // Apply moving average smoothing to rate
                LOG_INF("Rate calc: count=%d, interval=%dms, raw_rate=%.1f", 
                        widget->reception_count, interval_ms, current_rate_hz);
                widget->last_rate_hz = calculate_smoothed_rate(widget, current_rate_hz);
            }
            // Reset for next measurement period
            widget->reception_count = 0; // Reset counter - this reception was already counted
            widget->last_display_update = now;
        }
        // If less than 3 seconds, accumulate reception count for next calculation
    } else {
        // First reception - initialize more conservatively
        widget->last_rate_hz = 0.5f;  // Start with conservative 0.5Hz estimate
        widget->reception_count = 1;  // Count this first reception
        widget->last_display_update = now;
    }

    // Update RSSI bar using smoothed value
    uint8_t bars = rssi_to_bars(smoothed_rssi);
    lv_bar_set_value(widget->rssi_bar, bars, LV_ANIM_OFF);
    lv_color_t rssi_color = get_rssi_color(bars);
    lv_obj_set_style_bg_color(widget->rssi_bar, rssi_color, LV_PART_INDICATOR);

    // Update RSSI value label with smoothed value
    lv_label_set_text_fmt(widget->rssi_label, "%ddBm", smoothed_rssi);

    // Update reception rate label with 1Hz for responsive display
    static uint32_t last_rate_display_update = 0;
    if ((now - last_rate_display_update) >= 1000) {  // 1Hz display updates for responsiveness
        if (widget->last_rate_hz > 0.1f) {  // Only show rate if > 0.1Hz
            char rate_text[16];
            int rate_int = (int)(widget->last_rate_hz * 10);  // Convert to tenths
            snprintf(rate_text, sizeof(rate_text), "%d.%dHz", rate_int / 10, rate_int % 10);
            lv_label_set_text(widget->rate_label, rate_text);
        } else {
            lv_label_set_text(widget->rate_label, "--Hz");
        }
        last_rate_display_update = now;
    }

    LOG_DBG("Signal update: raw=%ddBm smoothed=%ddBm (%d bars), rate=%.1fHz", 
            rssi, smoothed_rssi, bars, widget->last_rate_hz);
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
    widget->last_display_update = 0;
    widget->last_rate_hz = 0;
    widget->reception_count = 0;
    widget->interval_start = 0;
    
    // Initialize RSSI smoothing
    widget->rssi_sample_index = 0;
    widget->rssi_sample_count = 0;
    widget->rssi_smoothed = 0;
    for (uint8_t i = 0; i < RSSI_SMOOTHING_SAMPLES; i++) {
        widget->rssi_samples[i] = 0;
    }
    
    // Initialize rate smoothing
    widget->rate_sample_index = 0;
    widget->rate_sample_count = 0;
    widget->rate_smoothed = 0.0f;
    for (uint8_t i = 0; i < RATE_SMOOTHING_SAMPLES; i++) {
        widget->rate_samples[i] = 0.0f;
    }
    
    // Initialize timeout detection
    widget->last_signal_time = 0;
    widget->signal_active = false;

    LOG_INF("Signal status widget initialized (RSSI + reception rate)");
    return 0;
}

void zmk_widget_signal_status_reset(struct zmk_widget_signal_status *widget) {
    if (!widget || !widget->rssi_bar || !widget->rssi_label || !widget->rate_label) {
        return;
    }
    
    LOG_INF("Signal widget reset - clearing all signal status");
    
    // Reset RSSI bar to minimum
    lv_bar_set_value(widget->rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    
    // Reset RSSI label
    lv_label_set_text(widget->rssi_label, "--dBm");
    
    // Reset rate label
    lv_label_set_text(widget->rate_label, "--Hz");
    
    // Reset cached values
    widget->last_update_time = 0;
    widget->last_display_update = 0;
    widget->last_rate_hz = 0.0f;
    widget->reception_count = 0;
    widget->interval_start = 0;
    
    // Reset RSSI smoothing buffer
    widget->rssi_sample_index = 0;
    widget->rssi_sample_count = 0;
    widget->rssi_smoothed = 0;
    for (uint8_t i = 0; i < RSSI_SMOOTHING_SAMPLES; i++) {
        widget->rssi_samples[i] = 0;
    }
    
    // Reset rate smoothing buffer
    widget->rate_sample_index = 0;
    widget->rate_sample_count = 0;
    widget->rate_smoothed = 0.0f;
    for (uint8_t i = 0; i < RATE_SMOOTHING_SAMPLES; i++) {
        widget->rate_samples[i] = 0.0f;
    }
    
    // Reset timeout detection
    widget->last_signal_time = 0;
    widget->signal_active = false;
}

lv_obj_t *zmk_widget_signal_status_obj(struct zmk_widget_signal_status *widget) {
    return widget ? widget->obj : NULL;
}

// Public function to check for signal timeout (called periodically)
void zmk_widget_signal_status_check_timeout(struct zmk_widget_signal_status *widget) {
    if (!widget || !widget->obj) {
        return;
    }
    
    check_signal_timeout(widget);
}

// Periodic 1Hz update function - called every second regardless of signal reception
void zmk_widget_signal_status_periodic_update(struct zmk_widget_signal_status *widget) {
    if (!widget || !widget->obj) {
        return;
    }
    
    uint32_t now = k_uptime_get_32();
    
    // Force timeout check every second
    check_signal_timeout(widget);
    
    // If signal is still active, check if we need to update rate calculation
    if (widget->signal_active) {
        // Always update rate calculation every second for accurate real-time display
        uint32_t time_since_last_calc = now - widget->last_display_update;
        
        // Update with 3-second window for responsive yet stable calculation
        // Combined with 10-sample smoothing for maximum stability
        if (time_since_last_calc >= 3000) {  // 3-second calculation window
            if (widget->reception_count == 0) {
                // No receptions in this period - IMMEDIATELY drop rate toward 0
                // Use VERY aggressive decay for no reception periods (especially for IDLE mode)
                widget->last_rate_hz = widget->last_rate_hz * 0.1f;  // Very quick decay (90% reduction per second)
                
                LOG_INF("No reception in 1s interval - decaying rate to %.1fHz", 
                        widget->last_rate_hz);
            } else {
                // Calculate rate based on actual receptions in this period
                float current_rate = (widget->reception_count * 1000.0f) / time_since_last_calc;
                
                // For non-zero rates, use less aggressive smoothing to be more responsive
                // Weight current measurement more heavily (80/20 instead of moving average)
                widget->last_rate_hz = (current_rate * 0.8f) + (widget->last_rate_hz * 0.2f);
                
                LOG_INF("Periodic rate update (3s window, 10-sample smooth): %d receptions in %dms = %.1fHz, smoothed to %.1fHz", 
                        widget->reception_count, time_since_last_calc, current_rate, widget->last_rate_hz);
            }
            
            // Update rate display with current rate
            if (widget->last_rate_hz > 0.05f) {  // Lower threshold for better IDLE detection
                char rate_text[16];
                int rate_int = (int)(widget->last_rate_hz * 10);
                snprintf(rate_text, sizeof(rate_text), "%d.%dHz", rate_int / 10, rate_int % 10);
                lv_label_set_text(widget->rate_label, rate_text);
            } else {
                // Rate has declined to nearly zero
                lv_label_set_text(widget->rate_label, "0.0Hz");
                widget->last_rate_hz = 0.0f;  // Fully reset when reaching near-zero
            }
            
            // Reset for next measurement period
            widget->reception_count = 0;
            widget->last_display_update = now;
        }
        // If less than 1 second, don't update - let individual receptions handle immediate display
        
    } else {
        // No signal active - ensure display shows no activity
        lv_label_set_text(widget->rssi_label, "--dBm");
        lv_label_set_text(widget->rate_label, "--Hz");
        lv_bar_set_value(widget->rssi_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(widget->rssi_bar, lv_color_make(0x60, 0x60, 0x60), LV_PART_INDICATOR);
    }
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY