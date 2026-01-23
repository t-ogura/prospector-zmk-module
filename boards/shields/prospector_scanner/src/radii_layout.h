/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Radii Layout for Scanner Mode
 * Port of carrefinho's feat/new-status-screens radii layout
 *
 * Features:
 * - 4 color palettes: Blue, Green, Red, Purple
 * - Mac-style modifier symbols (⌘, ⌥, ⌃, ⇧)
 * - Rotating layer wheel indicator
 */

#pragma once

#include <lvgl.h>

/* Radii layout API */
lv_obj_t *radii_layout_create(lv_obj_t *parent);
void radii_layout_update(uint8_t active_layer, const char *layer_name,
                        uint8_t battery_level, bool battery_connected,
                        uint8_t peripheral_battery, bool peripheral_connected,
                        uint8_t modifier_flags, bool usb_connected, uint8_t ble_profile);
void radii_layout_destroy(void);

/* Color palette cycling (Blue -> Green -> Red -> Purple -> Blue) */
void radii_layout_cycle_palette(void);
uint8_t radii_layout_get_palette(void);
const char *radii_layout_get_palette_name(void);
