/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>

/**
 * Touch event data structure
 */
struct touch_event_data {
    uint16_t x;          // Touch X coordinate (0-239)
    uint16_t y;          // Touch Y coordinate (0-279)
    bool touched;        // Touch state (true = touched, false = released)
    uint32_t timestamp;  // Event timestamp (ms)
};

/**
 * Touch event callback type
 *
 * @param event Touch event data
 */
typedef void (*touch_event_callback_t)(const struct touch_event_data *event);

/**
 * Initialize touch handler
 *
 * @return 0 on success, negative errno on failure
 */
int touch_handler_init(void);

/**
 * Register a callback for touch events
 *
 * @param callback Function to call when touch event occurs
 * @return 0 on success, negative errno on failure
 */
int touch_handler_register_callback(touch_event_callback_t callback);

/**
 * Get last touch event data
 *
 * @param event Pointer to store event data
 * @return 0 on success, -ENODATA if no event available
 */
int touch_handler_get_last_event(struct touch_event_data *event);
