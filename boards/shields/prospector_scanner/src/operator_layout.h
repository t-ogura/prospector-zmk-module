/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Operator Layout for Scanner Mode
 * Direct port from carrefinho's feat/new-status-screens
 */

#pragma once

#include <lvgl.h>

/* Display colors from carrefinho's operator/display_colors.h */
#define DISPLAY_COLOR_MOD_ACTIVE       0xb1e5f0
#define DISPLAY_COLOR_MOD_INACTIVE     0x3b527c
#define DISPLAY_COLOR_MOD_SEPARATOR    0x606060
#define DISPLAY_COLOR_MOD_CAPS_WORD    0xffbf00

#define DISPLAY_COLOR_WPM_BAR_ACTIVE   0xc2526a
#define DISPLAY_COLOR_WPM_BAR_INACTIVE 0x242424
#define DISPLAY_COLOR_WPM_TEXT         0xc2526a

#define DISPLAY_COLOR_LAYER_TEXT       0xffffff
#define DISPLAY_COLOR_LAYER_DOT_ACTIVE   0xe0e0e0
#define DISPLAY_COLOR_LAYER_DOT_INACTIVE 0x575757

#define DISPLAY_COLOR_BATTERY_FILL     0x54806c
#define DISPLAY_COLOR_BATTERY_RING     0x2a4036
#define DISPLAY_COLOR_BATTERY_BG       0x505050
#define DISPLAY_COLOR_BATTERY_LABEL    0xffffff

#define DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL  0x383c42
#define DISPLAY_COLOR_BATTERY_DISCONNECTED_RING  0x282c30
#define DISPLAY_COLOR_BATTERY_DISCONNECTED_LABEL 0x000000

#define DISPLAY_COLOR_BATTERY_LOW_FILL  0xC08040
#define DISPLAY_COLOR_BATTERY_LOW_RING  0x584028

#define DISPLAY_COLOR_USB_ACTIVE_BG        0xb9b9a7
#define DISPLAY_COLOR_USB_INACTIVE_BG      0x4F4F40
#define DISPLAY_COLOR_BLE_ACTIVE_BG        0x569FA7
#define DISPLAY_COLOR_BLE_INACTIVE_BG      0x353f40
#define DISPLAY_COLOR_OUTPUT_ACTIVE_TEXT   0x000000
#define DISPLAY_COLOR_OUTPUT_INACTIVE_TEXT 0x7b7d93

#define DISPLAY_COLOR_SLOT_ACTIVE_BG   0x7b7d93
#define DISPLAY_COLOR_SLOT_INACTIVE_BG 0x353640
#define DISPLAY_COLOR_SLOT_TEXT        0xffffff

/* Constants */
#define WPM_BAR_COUNT 26
#define WPM_MAX 120
#define LAYER_DOT_MAX 16  /* Maximum supported layers */
#define LOW_BATTERY_THRESHOLD 20
#define ARC_WIDTH_CONNECTED 6
#define ARC_WIDTH_DISCONNECTED 2

/* BLE Profile Connection States */
typedef enum {
    BLE_PROFILE_STATE_UNREGISTERED = 0,  /* Not bonded - waiting for pairing */
    BLE_PROFILE_STATE_REGISTERED,         /* Bonded but not connected */
    BLE_PROFILE_STATE_CONNECTED           /* Bonded and connected */
} ble_profile_state_t;

/* Operator layout API */
lv_obj_t *operator_layout_create(lv_obj_t *parent);
void operator_layout_update(uint8_t active_layer, const char *layer_name,
                           uint8_t battery_level, bool battery_connected,
                           uint8_t peripheral_battery, bool peripheral_connected,
                           uint8_t wpm, uint8_t modifier_flags,
                           bool usb_connected, uint8_t ble_profile,
                           bool ble_connected, bool ble_bonded);
void operator_layout_destroy(void);

/* Color palette cycling (Teal -> Warm -> Purple -> Mono -> Teal) */
void operator_layout_cycle_palette(void);
uint8_t operator_layout_get_palette(void);
const char *operator_layout_get_palette_name(void);
