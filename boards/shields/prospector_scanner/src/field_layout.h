/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FIELD Layout for Scanner Mode
 * Port of carrefinho's feat/new-status-screens field layout
 *
 * Features:
 * - Animated line segment grid (8x6, responds to WPM)
 * - Layer name display (top-left)
 * - Modifier indicators (right column)
 * - Battery label (bottom-left)
 */

#pragma once

#include <lvgl.h>

/* Field layout API */
lv_obj_t *field_layout_create(lv_obj_t *parent);
void field_layout_update(uint8_t active_layer, const char *layer_name,
                         uint8_t battery_level, bool battery_connected,
                         uint8_t peripheral_battery, bool peripheral_connected,
                         uint8_t wpm, uint8_t modifier_flags,
                         bool usb_connected, uint8_t ble_profile,
                         bool ble_connected, bool ble_bonded);
void field_layout_destroy(void);

/* Color palette cycling */
void field_layout_cycle_palette(void);
uint8_t field_layout_get_palette(void);
const char *field_layout_get_palette_name(void);
