/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file periodic_adv_protocol.h
 * @brief Prospector Periodic Advertising Protocol v2.2.0
 *
 * This protocol uses BLE Periodic Advertising to send extended keyboard status
 * data from keyboards to scanners. Data is split into two packet types:
 *
 * - Dynamic Packet (36 bytes, 30ms): High-frequency data (layer, mods, wpm, pointer)
 * - Static Packet (128 bytes, 5s): Low-frequency data (layer names, rssi, config)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Prospector Signature (for packet identification)
 *============================================================================*/

/**
 * @brief Prospector packet signature
 *
 * All Prospector packets start with this 4-byte signature:
 * - manufacturer_id: 0xFF 0xFF (BLE SIG unassigned)
 * - service_uuid: 0xAB 0xCE (Prospector v2.2.0 Periodic)
 *
 * Legacy packets use 0xAB 0xCD, so scanners can differentiate:
 * - 0xAB 0xCD = Legacy v1 (26-byte status_adv_data)
 * - 0xAB 0xCE = v2.2.0 Periodic (dynamic/static packets)
 */
#define PROSPECTOR_SIGNATURE_0        0xFF
#define PROSPECTOR_SIGNATURE_1        0xFF
#define PROSPECTOR_SERVICE_UUID_0     0xAB
#define PROSPECTOR_SERVICE_UUID_1     0xCE  /* 0xCE = v2.2.0, 0xCD = Legacy */

/*============================================================================
 * Packet Types
 *============================================================================*/

#define PERIODIC_PACKET_TYPE_DYNAMIC  0x01
#define PERIODIC_PACKET_TYPE_STATIC   0x02

/*============================================================================
 * Dynamic Packet (40 bytes, 30ms interval)
 *============================================================================*/

#define DYNAMIC_PACKET_SIZE           40
#define LAYER_NAME_MAX_LEN            8

/**
 * @brief status_flags bit definitions
 */
#define STATUS_FLAG_CAPS_WORD         (1 << 0)
#define STATUS_FLAG_CHARGING          (1 << 1)
#define STATUS_FLAG_USB_CONNECTED     (1 << 2)
#define STATUS_FLAG_USB_HID_READY     (1 << 3)
#define STATUS_FLAG_BLE_CONNECTED     (1 << 4)
#define STATUS_FLAG_BLE_BONDED        (1 << 5)
#define STATUS_FLAG_HAS_POINTING      (1 << 6)

/**
 * @brief indicator_flags bit definitions (Caps/Num/Scroll Lock + Sticky Keys)
 */
#define INDICATOR_FLAG_CAPS_LOCK      (1 << 0)
#define INDICATOR_FLAG_NUM_LOCK       (1 << 1)
#define INDICATOR_FLAG_SCROLL_LOCK    (1 << 2)
#define INDICATOR_FLAG_STICKY_SHIFT   (1 << 3)
#define INDICATOR_FLAG_STICKY_CTRL    (1 << 4)
#define INDICATOR_FLAG_STICKY_ALT     (1 << 5)
#define INDICATOR_FLAG_STICKY_GUI     (1 << 6)

/**
 * @brief BLE Profile Status (2 bits per profile)
 */
#define BLE_PROFILE_UNUSED            0x00
#define BLE_PROFILE_CONNECTED         0x01
#define BLE_PROFILE_BONDED            0x02

/**
 * @brief Pointer button bit definitions
 */
#define POINTER_BTN_LEFT              (1 << 0)
#define POINTER_BTN_RIGHT             (1 << 1)
#define POINTER_BTN_MIDDLE            (1 << 2)
#define POINTER_BTN_BACK              (1 << 3)
#define POINTER_BTN_FORWARD           (1 << 4)

/**
 * @brief peripheral_status bit definitions
 */
#define PERIPHERAL_STATUS_0_CONNECTED (1 << 0)
#define PERIPHERAL_STATUS_1_CONNECTED (1 << 1)
#define PERIPHERAL_STATUS_2_CONNECTED (1 << 2)

/**
 * @brief Dynamic packet structure (40 bytes)
 *
 * Contains high-frequency data that changes often:
 * - Active layer and name
 * - Modifier keys
 * - WPM
 * - Battery levels
 * - Pointer movement
 *
 * Starts with 4-byte Prospector signature for identification.
 */
struct periodic_dynamic_packet {
    /* Prospector signature (4 bytes) */
    uint8_t  manufacturer_id[2];    /* 0xFF 0xFF */
    uint8_t  service_uuid[2];       /* 0xAB 0xCE (v2.2.0) */

    /* Packet identification */
    uint8_t  packet_type;           /* PERIODIC_PACKET_TYPE_DYNAMIC (0x01) */

    /* Core status data */
    uint8_t  active_layer;
    uint8_t  modifier_flags;
    uint8_t  status_flags;
    uint8_t  wpm_value;
    uint8_t  battery_level;
    uint8_t  peripheral_battery[3];
    uint8_t  profile_slot;
    uint8_t  peripheral_status;
    uint8_t  connection_count;
    uint16_t ble_profile_flags;
    uint16_t sequence_number;
    char     current_layer_name[LAYER_NAME_MAX_LEN];

    /* Pointer/trackball data */
    int16_t  pointer_dx;
    int16_t  pointer_dy;
    int8_t   scroll_v;
    int8_t   scroll_h;
    uint8_t  pointer_buttons;

    /* Additional status */
    uint8_t  idle_seconds_div4;
    uint8_t  indicator_flags;
    uint8_t  reserved[3];
} __packed;

_Static_assert(sizeof(struct periodic_dynamic_packet) == DYNAMIC_PACKET_SIZE,
               "Dynamic packet must be exactly 40 bytes");

/*============================================================================
 * Static Packet (132 bytes, 5 second interval)
 *============================================================================*/

#define STATIC_PACKET_SIZE            132
#define STATIC_LAYER_COUNT            10
#define KEYBOARD_NAME_MAX_LEN         24

/**
 * @brief RSSI invalid value (peripheral not connected)
 */
#define RSSI_INVALID                  0x7F  /* 127 = not connected */

/**
 * @brief device_features bit definitions
 */
#define DEVICE_FEATURE_TRACKBALL      (1 << 0)
#define DEVICE_FEATURE_TRACKPAD       (1 << 1)
#define DEVICE_FEATURE_ENCODER        (1 << 2)
#define DEVICE_FEATURE_DISPLAY        (1 << 3)
#define DEVICE_FEATURE_RGB            (1 << 4)
#define DEVICE_FEATURE_BACKLIGHT      (1 << 5)

/**
 * @brief Static packet structure (132 bytes)
 *
 * Contains low-frequency data that rarely changes:
 * - Keyboard name and ID
 * - Layer names (up to 10 layers)
 * - Device features
 * - Peripheral RSSI
 *
 * Starts with 4-byte Prospector signature for identification.
 */
struct periodic_static_packet {
    /* Prospector signature (4 bytes) */
    uint8_t  manufacturer_id[2];    /* 0xFF 0xFF */
    uint8_t  service_uuid[2];       /* 0xAB 0xCE (v2.2.0) */

    /* Packet identification */
    uint8_t  packet_type;           /* PERIODIC_PACKET_TYPE_STATIC (0x02) */
    uint8_t  static_version;        /* Currently: 1 */

    /* Keyboard identity */
    uint32_t keyboard_id;
    uint8_t  layer_count;
    uint8_t  device_role;
    char     keyboard_name[KEYBOARD_NAME_MAX_LEN];

    /* Firmware info */
    uint16_t firmware_version;      /* (major << 8) | minor */
    uint8_t  device_features;
    uint8_t  reserved_header;

    /* Layer names (10 layers × 8 chars = 80 bytes) */
    char     layer_names[STATIC_LAYER_COUNT][LAYER_NAME_MAX_LEN];

    /* Statistics */
    uint32_t total_keypress;
    uint16_t boot_count;
    uint16_t zephyr_version;        /* (major << 8) | minor */

    /* Peripheral info */
    int8_t   peripheral_rssi[3];    /* dBm, 0x7F = invalid */
    uint8_t  reserved;
} __packed;

_Static_assert(sizeof(struct periodic_static_packet) == STATIC_PACKET_SIZE,
               "Static packet must be exactly 132 bytes");

/*============================================================================
 * Helper Macros
 *============================================================================*/

/**
 * @brief Get BLE profile status from flags
 */
#define BLE_PROFILE_STATUS_GET(flags, profile) \
    (((flags) >> ((profile) * 2)) & 0x03)

/**
 * @brief Set BLE profile status in flags
 */
#define BLE_PROFILE_STATUS_SET(flags, profile, status) \
    do { \
        (flags) &= ~(0x03 << ((profile) * 2)); \
        (flags) |= ((status) & 0x03) << ((profile) * 2); \
    } while (0)

/**
 * @brief Encode firmware version
 */
#define FW_VERSION_ENCODE(major, minor)  (((major) << 8) | (minor))
#define FW_VERSION_MAJOR(ver)            ((ver) >> 8)
#define FW_VERSION_MINOR(ver)            ((ver) & 0xFF)

/**
 * @brief Idle time conversion (seconds to div4 format)
 */
#define IDLE_SECONDS_TO_DIV4(sec)        (((sec) > 1020) ? 255 : ((sec) / 4))
#define IDLE_DIV4_TO_SECONDS(div4)       ((div4) * 4)

/**
 * @brief Check if RSSI value is valid
 */
#define RSSI_IS_VALID(rssi)              ((rssi) != RSSI_INVALID)

/*============================================================================
 * Protocol Version
 *============================================================================*/

#define PERIODIC_ADV_PROTOCOL_VERSION    1
#define PERIODIC_ADV_STATIC_VERSION      1

/*============================================================================
 * Default Intervals
 *============================================================================*/

#ifndef CONFIG_PROSPECTOR_DYNAMIC_PACKET_INTERVAL_MS
#define CONFIG_PROSPECTOR_DYNAMIC_PACKET_INTERVAL_MS  30
#endif

#ifndef CONFIG_PROSPECTOR_STATIC_PACKET_INTERVAL_MS
#define CONFIG_PROSPECTOR_STATIC_PACKET_INTERVAL_MS   5000
#endif

/*============================================================================
 * API Functions
 *============================================================================*/

/**
 * @brief Initialize periodic advertising protocol
 * @return 0 on success, negative error code on failure
 */
int periodic_adv_protocol_init(void);

/**
 * @brief Start periodic advertising
 * @return 0 on success, negative error code on failure
 */
int periodic_adv_protocol_start(void);

/**
 * @brief Stop periodic advertising
 * @return 0 on success, negative error code on failure
 */
int periodic_adv_protocol_stop(void);

/**
 * @brief Build and get dynamic packet
 * @param packet Pointer to dynamic packet structure to fill
 * @return 0 on success, negative error code on failure
 */
int periodic_adv_build_dynamic_packet(struct periodic_dynamic_packet *packet);

/**
 * @brief Build and get static packet
 * @param packet Pointer to static packet structure to fill
 * @return 0 on success, negative error code on failure
 */
int periodic_adv_build_static_packet(struct periodic_static_packet *packet);

/**
 * @brief Force immediate static packet transmission
 *
 * Call this when static data changes (e.g., layer name update)
 */
void periodic_adv_request_static_update(void);

/**
 * @brief Update pointer movement accumulator
 *
 * Called from pointing device driver to accumulate movement
 *
 * @param dx X movement delta
 * @param dy Y movement delta
 */
void periodic_adv_update_pointer(int16_t dx, int16_t dy);

/**
 * @brief Update scroll accumulator
 *
 * Called from pointing device driver to accumulate scroll
 *
 * @param v Vertical scroll delta
 * @param h Horizontal scroll delta
 */
void periodic_adv_update_scroll(int8_t v, int8_t h);

/**
 * @brief Update pointer button state
 *
 * @param buttons Button state bitfield
 */
void periodic_adv_update_pointer_buttons(uint8_t buttons);

#ifdef __cplusplus
}
#endif
