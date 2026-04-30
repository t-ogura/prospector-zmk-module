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
    uint8_t version;               // [7:4]=module major, [3:0]=module minor
    uint8_t battery_level;         // Central/Standalone battery level 0-100%
    uint8_t active_layer;          // Current active layer 0-15
    uint8_t profile_slot;          // [7]=unused, [6]=dev flag, [5:3]=patch, [2:0]=profile 0-4
    uint8_t connection_count;      // Number of connected devices 0-5
    uint8_t status_flags;          // Status flags (bit field)
    uint8_t device_role;           // Device role (CENTRAL/PERIPHERAL/STANDALONE)
    uint8_t device_index;          // Device index for split keyboards
    uint8_t peripheral_battery[3]; // Battery levels: [0]=Left keyboard, [1]=Right/Aux, [2]=Third device (0=N/A)
    char layer_name[4];            // Layer name (null-terminated, reduced from 6 to 4)
    uint8_t keyboard_id[4];        // Keyboard identifier
    uint8_t modifier_flags;        // Active modifier keys (Ctrl/Shift/Alt/GUI)
    uint8_t wpm_value;             // Words per minute (0-255, 0 = inactive/unknown)
    uint8_t channel;               // Channel number 0-255 (0 = accept all/broadcast to all)
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
 * @brief Protocol version (legacy, kept for reference)
 */
#define ZMK_STATUS_ADV_VERSION 1

/**
 * @brief Module version encoding
 *
 * version byte:      [7:4] = major (0-15), [3:0] = minor (0-15)
 * profile_slot byte: [6] = dev flag, [5:3] = patch (0-7), [2:0] = profile (0-4)
 *
 * Backward compatible: old scanners read profile_slot & 0x07 implicitly
 * (patch=0, dev=0 leaves upper bits zero for release versions)
 */
/* Ensure STRINGIFY is available */
#ifndef STRINGIFY
#define _PROSPECTOR_STRINGIFY(x) #x
#define STRINGIFY(x) _PROSPECTOR_STRINGIFY(x)
#endif

#define PROSPECTOR_MODULE_VERSION_MAJOR  2
#define PROSPECTOR_MODULE_VERSION_MINOR  2
#define PROSPECTOR_MODULE_VERSION_PATCH  0
#define PROSPECTOR_MODULE_VERSION_DEV    0  /* 0=release, 1=dev */

#if PROSPECTOR_MODULE_VERSION_DEV
#define PROSPECTOR_VERSION_STRING \
    STRINGIFY(PROSPECTOR_MODULE_VERSION_MAJOR) "." \
    STRINGIFY(PROSPECTOR_MODULE_VERSION_MINOR) "." \
    STRINGIFY(PROSPECTOR_MODULE_VERSION_PATCH) "-dev"
#else
#define PROSPECTOR_VERSION_STRING \
    STRINGIFY(PROSPECTOR_MODULE_VERSION_MAJOR) "." \
    STRINGIFY(PROSPECTOR_MODULE_VERSION_MINOR) "." \
    STRINGIFY(PROSPECTOR_MODULE_VERSION_PATCH)
#endif

#define PROSPECTOR_ENCODE_VERSION() \
    ((PROSPECTOR_MODULE_VERSION_MAJOR << 4) | PROSPECTOR_MODULE_VERSION_MINOR)

#define PROSPECTOR_ENCODE_PROFILE_SLOT(profile) \
    (((PROSPECTOR_MODULE_VERSION_DEV & 0x01) << 6) | \
     ((PROSPECTOR_MODULE_VERSION_PATCH & 0x07) << 3) | \
     ((profile) & 0x07))

/* Decoder macros (for scanner side) */
#define PROSPECTOR_DECODE_VERSION_MAJOR(ver)    ((ver) >> 4)
#define PROSPECTOR_DECODE_VERSION_MINOR(ver)    ((ver) & 0x0F)
#define PROSPECTOR_DECODE_PATCH(prof_slot)      (((prof_slot) >> 3) & 0x07)
#define PROSPECTOR_DECODE_DEV(prof_slot)        (((prof_slot) >> 6) & 0x01)
#define PROSPECTOR_DECODE_PROFILE(prof_slot)    ((prof_slot) & 0x07)

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