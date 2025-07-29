/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Power states
enum scanner_power_state {
    SCANNER_STATE_ACTIVE,     // Normal operation
    SCANNER_STATE_IDLE,       // Dimmed display, reduced scan rate
    SCANNER_STATE_STANDBY,    // Display off, minimal scanning
    SCANNER_STATE_SLEEP       // Deep sleep, no scanning
};

/**
 * @brief Update activity timestamp and wake scanner if needed
 */
void scanner_power_mgmt_activity(void);

/**
 * @brief Manually set power state
 * 
 * @param state Target power state
 */
void scanner_power_mgmt_set_state(enum scanner_power_state state);

/**
 * @brief Get current power state
 * 
 * @return Current power state
 */
enum scanner_power_state scanner_power_mgmt_get_state(void);

/**
 * @brief Set display brightness
 * 
 * @param brightness Brightness percentage (0-100)
 * @return 0 on success, negative error code on failure
 */
int zmk_display_set_brightness(uint8_t brightness);

/**
 * @brief Get current display brightness
 * 
 * @return Current brightness percentage (0-100)
 */
uint8_t zmk_display_get_brightness(void);

#ifdef __cplusplus
}
#endif