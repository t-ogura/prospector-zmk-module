/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <lvgl.h>
#include <zmk/status_scanner.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSSI_SMOOTHING_SAMPLES 5  // Moving average window size for RSSI
#define RATE_SMOOTHING_SAMPLES 10  // 10-second smoothing buffer for stable rate display

struct zmk_widget_signal_status {
    lv_obj_t *obj;
    lv_obj_t *rssi_bar;        // RSSI strength bar (0-5 bars)
    lv_obj_t *rssi_label;      // RSSI dBm value
    lv_obj_t *rate_label;      // Reception rate in Hz
    uint32_t last_update_time; // For calculating reception rate
    uint32_t last_display_update; // For display rate limiting
    float last_rate_hz;        // Cached reception rate
    uint32_t reception_count;   // Number of receptions in current interval
    uint32_t interval_start;    // Start of current measurement interval
    
    // RSSI smoothing using moving average
    int8_t rssi_samples[RSSI_SMOOTHING_SAMPLES];
    uint8_t rssi_sample_index;
    uint8_t rssi_sample_count;
    int8_t rssi_smoothed;
    
    // Rate Hz smoothing using moving average
    float rate_samples[RATE_SMOOTHING_SAMPLES];
    uint8_t rate_sample_index;
    uint8_t rate_sample_count;
    float rate_smoothed;
    
    // Timeout detection for clearing stale values
    uint32_t last_signal_time; // Last time a signal was received
    bool signal_active;        // Whether we have active signal
};

int zmk_widget_signal_status_init(struct zmk_widget_signal_status *widget, lv_obj_t *parent);
void zmk_widget_signal_status_update(struct zmk_widget_signal_status *widget, int8_t rssi);
void zmk_widget_signal_status_reset(struct zmk_widget_signal_status *widget);
void zmk_widget_signal_status_check_timeout(struct zmk_widget_signal_status *widget);
void zmk_widget_signal_status_periodic_update(struct zmk_widget_signal_status *widget);
lv_obj_t *zmk_widget_signal_status_obj(struct zmk_widget_signal_status *widget);

#ifdef __cplusplus
}
#endif