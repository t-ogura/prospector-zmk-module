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
