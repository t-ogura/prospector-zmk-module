/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/status_scanner.h>
#include <zmk/status_advertisement.h>

/**
 * @brief Send keyboard data received from BLE advertisement
 *
 * Lock-free: pushes data into SPSC ring buffer.
 * Called from BT RX thread. Data is processed by scanner_process_incoming()
 * in the LVGL timer context.
 *
 * @param adv_data Parsed advertisement data
 * @param rssi Signal strength
 * @param device_name Keyboard device name
 * @param ble_addr BLE MAC address (6 bytes)
 * @param ble_addr_type BLE address type
 * @return 0 on success, negative error code on failure
 */
int scanner_msg_send_keyboard_data(const struct zmk_status_adv_data *adv_data,
                                   int8_t rssi, const char *device_name,
                                   const uint8_t *ble_addr, uint8_t ble_addr_type);

/**
 * @brief Process incoming advertisements from ring buffer
 *
 * Must be called from LVGL timer context (main thread).
 * Drains ring buffer, manages keyboards[], calculates rates,
 * checks timeouts, and updates scanner battery.
 */
void scanner_process_incoming(void);

/**
 * @brief Trigger timeout check for keyboards
 *
 * Checks if any keyboards have timed out and updates display accordingly.
 *
 * @return 0 on success, negative error code on failure
 */
int scanner_msg_send_timeout_check(void);

/**
 * @brief Get keyboard data by index
 *
 * @param index Keyboard index
 * @param data Output: advertisement data
 * @param rssi Output: signal strength
 * @param name Output: keyboard name
 * @param name_len Size of name buffer
 * @return true if keyboard found, false otherwise
 */
bool scanner_get_keyboard_data(int index, struct zmk_status_adv_data *data,
                               int8_t *rssi, char *name, size_t name_len);

/**
 * @brief Get the count of active keyboards
 *
 * @return Number of active keyboards
 */
int scanner_get_active_keyboard_count(void);

/**
 * @brief Get keyboard status pointer by index (LVGL timer context only)
 *
 * Returns a direct pointer to the keyboard status struct.
 * Safe because keyboards[] is only accessed from LVGL timer context.
 *
 * @param index Keyboard index (0 to MAX_KEYBOARDS-1)
 * @return Pointer to keyboard status, NULL if inactive or invalid index
 */
struct zmk_keyboard_status *scanner_get_keyboard_status(int index);

/**
 * @brief Get the selected keyboard index
 *
 * @return Selected keyboard index
 */
int scanner_get_selected_keyboard(void);

/**
 * @brief Set the selected keyboard index
 *
 * @param index Keyboard index to select
 */
void scanner_set_selected_keyboard(int index);

/**
 * @brief Send display refresh request
 *
 * Triggers immediate display update after screen transitions to MAIN.
 *
 * @return 0 on success, negative error code on failure
 */
int scanner_msg_send_display_refresh(void);
