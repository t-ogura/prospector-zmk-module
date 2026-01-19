/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zmk/status_advertisement.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of keyboards that can be tracked
 */
#define ZMK_STATUS_SCANNER_MAX_KEYBOARDS CONFIG_PROSPECTOR_MAX_KEYBOARDS

/**
 * @brief Periodic sync state (v2.2.0)
 */
typedef enum {
    SYNC_STATE_NONE,      // v1 keyboard or not selected for sync
    SYNC_STATE_SYNCING,   // Sync establishment in progress
    SYNC_STATE_SYNCED,    // Periodic sync active
    SYNC_STATE_FALLBACK,  // Sync failed, using Legacy fallback
} sync_state_t;

/**
 * @brief Keyboard status information
 */
struct zmk_keyboard_status {
    bool active;                           // Whether this slot is active
    uint32_t last_seen;                    // Timestamp of last advertisement
    struct zmk_status_adv_data data;       // Latest status data
    int8_t rssi;                          // Signal strength
    char ble_name[32];                     // BLE device name from advertisement
    uint8_t ble_addr[6];                   // BLE MAC address for unique identification
    uint8_t ble_addr_type;                 // BLE address type (public/random)

    // Periodic Advertising support (v2.2.0)
    uint8_t sid;                           // Advertising Set ID (for Periodic sync)
    bool has_periodic;                     // Keyboard supports Periodic Advertising
};

/**
 * @brief Status scanner events
 */
enum zmk_status_scanner_event {
    ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND,
    ZMK_STATUS_SCANNER_EVENT_KEYBOARD_UPDATED,
    ZMK_STATUS_SCANNER_EVENT_KEYBOARD_LOST,
};

/**
 * @brief Status scanner event data
 */
struct zmk_status_scanner_event_data {
    enum zmk_status_scanner_event event;
    int keyboard_index;
    struct zmk_keyboard_status *status;
};

/**
 * @brief Status scanner callback function
 */
typedef void (*zmk_status_scanner_callback_t)(struct zmk_status_scanner_event_data *event_data);

/**
 * @brief Initialize the status scanner
 * 
 * @return 0 on success, negative error code on failure
 */
int zmk_status_scanner_init(void);

/**
 * @brief Start scanning for keyboard status advertisements
 * 
 * @return 0 on success, negative error code on failure
 */
int zmk_status_scanner_start(void);

/**
 * @brief Stop scanning for keyboard status advertisements
 * 
 * @return 0 on success, negative error code on failure
 */
int zmk_status_scanner_stop(void);

/**
 * @brief Register a callback for scanner events
 * 
 * @param callback Callback function to register
 * @return 0 on success, negative error code on failure
 */
int zmk_status_scanner_register_callback(zmk_status_scanner_callback_t callback);

/**
 * @brief Get keyboard status by index
 * 
 * @param index Keyboard index (0 to ZMK_STATUS_SCANNER_MAX_KEYBOARDS-1)
 * @return Pointer to keyboard status, NULL if invalid index
 */
struct zmk_keyboard_status *zmk_status_scanner_get_keyboard(int index);

/**
 * @brief Get the number of active keyboards
 * 
 * @return Number of active keyboards
 */
int zmk_status_scanner_get_active_count(void);

/**
 * @brief Get the index of the primary keyboard (most recently seen)
 *
 * @return Index of primary keyboard, -1 if no keyboards found
 */
int zmk_status_scanner_get_primary_keyboard(void);

// ============================================================================
// Periodic Sync API (v2.2.0)
// ============================================================================

/**
 * @brief Get current sync state for the selected keyboard
 *
 * @return Current sync state
 */
sync_state_t zmk_status_scanner_get_sync_state(void);

/**
 * @brief Select a keyboard and initiate sync (if v2)
 *
 * This function is called when the user selects a keyboard from the list.
 * If the keyboard supports Periodic Advertising, sync will be initiated.
 * Otherwise, Legacy mode continues.
 *
 * @param keyboard_index Index of the keyboard to select
 * @return 0 on success, negative error code on failure
 */
int zmk_status_scanner_select_keyboard(int keyboard_index);

/**
 * @brief Get the currently selected keyboard index
 *
 * @return Selected keyboard index, -1 if none selected
 */
int zmk_status_scanner_get_selected_keyboard(void);

/**
 * @brief Get sync status icon string for UI display
 *
 * @param state Sync state
 * @return Icon string ("   ", ">>>", "SYN", "LGC")
 */
const char* zmk_status_scanner_get_sync_icon(sync_state_t state);

#ifdef __cplusplus
}
#endif