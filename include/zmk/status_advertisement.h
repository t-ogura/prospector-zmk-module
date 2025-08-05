/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status advertisement data structure
 * 
 * This structure defines the data format for ZMK status advertisements.
 * BLE Legacy Advertising limit: Flags(3) + Manufacturer Data header(2) + Payload(26) = 31 bytes
 */
struct zmk_status_adv_data {
    uint8_t manufacturer_id[2];    // 0xFF, 0xFF (Company ID: 0xFFFF = Reserved)
    uint8_t service_uuid[2];       // 0xAB, 0xCD (Custom UUID for Prospector)
    uint8_t version;               // Protocol version
    uint8_t battery_level;         // Central/Standalone battery level 0-100%
    uint8_t active_layer;          // Current active layer 0-15
    uint8_t profile_slot;          // Active profile slot 0-4
    uint8_t connection_count;      // Number of connected devices 0-5
    uint8_t status_flags;          // Status flags (bit field)
    uint8_t device_role;           // Device role (CENTRAL/PERIPHERAL/STANDALONE)
    uint8_t device_index;          // Device index for split keyboards
    uint8_t peripheral_battery[3]; // Battery levels: [0]=Left keyboard, [1]=Right/Aux, [2]=Third device (0=N/A)
    char layer_name[4];            // Layer name (null-terminated, reduced from 6 to 4)
    uint8_t keyboard_id[4];        // Keyboard identifier
    uint8_t modifier_flags;        // Active modifier keys (Ctrl/Shift/Alt/GUI)
    uint8_t wpm_value;             // Words per minute (0-255, 0 = inactive/unknown)
    uint8_t reserved[1];           // Reserved for future use (reduced from 2 to 1)
} __packed;  // Total: 26 bytes

/**
 * @brief Status flags bit definitions
 */
#define ZMK_STATUS_FLAG_CAPS_WORD        (1 << 0)
#define ZMK_STATUS_FLAG_CHARGING         (1 << 1)
#define ZMK_STATUS_FLAG_USB_CONNECTED    (1 << 2)
#define ZMK_STATUS_FLAG_USB_HID_READY    (1 << 3)
#define ZMK_STATUS_FLAG_BLE_CONNECTED    (1 << 4)
#define ZMK_STATUS_FLAG_BLE_BONDED       (1 << 5)
// Bits 6-7 reserved for future use

/**
 * @brief Modifier key flags bit definitions (for modifier_flags field)
 */
#define ZMK_MOD_FLAG_LCTL    (1 << 0)  // Left Control
#define ZMK_MOD_FLAG_LSFT    (1 << 1)  // Left Shift
#define ZMK_MOD_FLAG_LALT    (1 << 2)  // Left Alt
#define ZMK_MOD_FLAG_LGUI    (1 << 3)  // Left GUI (Windows/Command)
#define ZMK_MOD_FLAG_RCTL    (1 << 4)  // Right Control
#define ZMK_MOD_FLAG_RSFT    (1 << 5)  // Right Shift
#define ZMK_MOD_FLAG_RALT    (1 << 6)  // Right Alt
#define ZMK_MOD_FLAG_RGUI    (1 << 7)  // Right GUI (Windows/Command)

/**
 * @brief Device role definitions for split keyboard support
 */
#define ZMK_DEVICE_ROLE_STANDALONE   0  // Regular keyboard
#define ZMK_DEVICE_ROLE_CENTRAL      1  // Split keyboard central side
#define ZMK_DEVICE_ROLE_PERIPHERAL   2  // Split keyboard peripheral side

/**
 * @brief Protocol version
 */
#define ZMK_STATUS_ADV_VERSION 1

/**
 * @brief Service UUID for Prospector status advertisement
 */
#define ZMK_STATUS_ADV_SERVICE_UUID 0xABCD

/**
 * @brief Initialize status advertisement
 * 
 * @return 0 on success, negative error code on failure
 */
int zmk_status_advertisement_init(void);

/**
 * @brief Update status advertisement data
 * 
 * This function updates the advertisement data with current keyboard status
 * and triggers a new advertisement broadcast.
 * 
 * @return 0 on success, negative error code on failure
 */
int zmk_status_advertisement_update(void);

/**
 * @brief Start status advertisement broadcasting
 * 
 * @return 0 on success, negative error code on failure
 */
int zmk_status_advertisement_start(void);

/**
 * @brief Stop status advertisement broadcasting
 * 
 * @return 0 on success, negative error code on failure
 */
int zmk_status_advertisement_stop(void);

#ifdef __cplusplus
}
#endif