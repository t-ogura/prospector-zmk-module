/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Display Settings - NVS Persistence for Scanner Display
 *
 * Persists user-configurable display settings across reboots using
 * Zephyr Settings Subsystem (NVS backend). Settings are loaded at boot.
 * Call display_settings_save_if_dirty() when leaving a settings screen.
 *
 * Settings stored:
 *   - Brightness (auto/manual mode, manual level)
 *   - Layer display (max layers, slide mode)
 *   - Scanner channel filter
 *   - Display layout style
 */

#ifndef DISPLAY_SETTINGS_H
#define DISPLAY_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize the display settings subsystem.
 * Loads saved settings from NVS flash. Call once during display init.
 */
void display_settings_init(void);

/**
 * Save settings to NVS if any value has changed since last save.
 * Call when leaving a settings screen (swipe away, screen transition).
 * No-op if nothing changed.
 */
void display_settings_save_if_dirty(void);

/* ========== Brightness ========== */

bool display_settings_get_auto_brightness(void);
void display_settings_set_auto_brightness(bool enabled);

uint8_t display_settings_get_manual_brightness(void);
void display_settings_set_manual_brightness(uint8_t level);

/* ========== Layer Display ========== */

uint8_t display_settings_get_max_layers(void);
void display_settings_set_max_layers(uint8_t max);

bool display_settings_get_layer_slide_mode(void);
void display_settings_set_layer_slide_mode(bool enabled);

/* ========== Scanner Channel ========== */

uint8_t display_settings_get_channel(void);
void display_settings_set_channel(uint8_t channel);

/* ========== Layout Style ========== */

uint8_t display_settings_get_layout(void);
void display_settings_set_layout(uint8_t layout);

/* ========== Scanner Battery Visibility ========== */

bool display_settings_get_battery_visible(void);
void display_settings_set_battery_visible(bool visible);

#endif /* DISPLAY_SETTINGS_H */
