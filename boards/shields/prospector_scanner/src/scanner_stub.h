/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/status_advertisement.h>

/**
 * @brief Send keyboard data received from BLE advertisement
 *
 * Thread-safe function to pass keyboard status data from BLE scan callback
 * to the display update work queue.
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
 * @brief Get keyboard data by BLE address
 *
 * @param ble_addr BLE MAC address (6 bytes)
 * @param data Output: advertisement data
 * @param rssi Output: signal strength
 * @param name Output: keyboard name
 * @param name_len Size of name buffer
 * @param found_index Output: index in local array where keyboard was found
 * @return true if keyboard found, false otherwise
 */
bool scanner_get_keyboard_data_by_addr(const uint8_t *ble_addr, struct zmk_status_adv_data *data,
                                        int8_t *rssi, char *name, size_t name_len, int *found_index);

/**
 * @brief Get the count of active keyboards
 *
 * @return Number of active keyboards
 */
int scanner_get_active_keyboard_count(void);

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
 * @brief Get the selected keyboard's BLE address
 *
 * @param ble_addr_out Output buffer for BLE address (6 bytes)
 * @return true if valid address available, false otherwise
 */
bool scanner_get_selected_keyboard_addr(uint8_t *ble_addr_out);

/**
 * @brief Update keyboard name by BLE address
 *
 * Called from status_scanner.c when SCAN_RSP with device name arrives.
 * Updates the local keyboard array entry if name is currently Unknown.
 *
 * @param ble_addr BLE MAC address (6 bytes)
 * @param name Device name to set
 */
void scanner_update_keyboard_name_by_addr(const uint8_t *ble_addr, const char *name);
