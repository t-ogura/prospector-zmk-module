/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Prospector Display Layouts - Inspired by carrefinho's feat/new-status-screens
 * Original layouts: https://github.com/carrefinho/prospector-zmk-module
 *
 * This implementation adapts the original designs for scanner mode,
 * using Periodic Advertising data instead of direct keyboard access.
 *
 * Layout Styles:
 * - CLASSIC: Large centered layer name with roller animation (FR_Regular_48 style)
 * - FIELD: Clean layout with layer name, battery bars, modifiers (FR_Regular_36 style)
 * - OPERATOR: Minimalist with dot indicators for layers
 * - RADII: Circular wheel-based layer indicator with rotation animation
 */

#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prospector display layout styles
 */
typedef enum {
    PROSPECTOR_LAYOUT_CLASSIC = 0,   /* Large roller-style layer display */
    PROSPECTOR_LAYOUT_FIELD,         /* Clean modern layout */
    PROSPECTOR_LAYOUT_OPERATOR,      /* Minimalist dot indicators */
    PROSPECTOR_LAYOUT_RADII,         /* Circular wheel indicator */
    PROSPECTOR_LAYOUT_COUNT
} prospector_layout_t;

/**
 * @brief Keyboard data for display (from Periodic ADV)
 */
struct prospector_keyboard_data {
    /* Dynamic data (from dynamic packet) */
    uint8_t active_layer;
    char current_layer_name[8];
    uint8_t modifier_flags;
    uint8_t wpm_value;
    uint8_t battery_level;
    uint8_t peripheral_battery[3];
    uint8_t profile_slot;
    uint8_t connection_count;
    uint8_t indicator_flags;
    bool ble_connected;
    bool ble_bonded;
    bool usb_connected;

    /* Static data (from static packet) */
    char keyboard_name[24];
    uint8_t layer_count;
    char layer_names[10][8];
    int8_t peripheral_rssi[3];

    /* Validity flags */
    bool has_dynamic_data;
    bool has_static_data;
};

/**
 * @brief Initialize prospector layouts module
 * @param parent Parent LVGL object for layouts
 */
void prospector_layouts_init(lv_obj_t *parent);

/**
 * @brief Destroy prospector layouts and free resources
 */
void prospector_layouts_destroy(void);

/**
 * @brief Set the current layout style
 * @param layout Layout style to display
 */
void prospector_layouts_set_style(prospector_layout_t layout);

/**
 * @brief Get the current layout style
 * @return Current layout style
 */
prospector_layout_t prospector_layouts_get_style(void);

/**
 * @brief Cycle to the next layout style
 */
void prospector_layouts_next(void);

/**
 * @brief Cycle to the previous layout style
 */
void prospector_layouts_prev(void);

/**
 * @brief Update display with new keyboard data
 * @param data Keyboard data from Periodic ADV
 */
void prospector_layouts_update(const struct prospector_keyboard_data *data);

/**
 * @brief Get the name of a layout style
 * @param layout Layout style
 * @return Human-readable name
 */
const char *prospector_layouts_get_name(prospector_layout_t layout);

#ifdef __cplusplus
}
#endif
