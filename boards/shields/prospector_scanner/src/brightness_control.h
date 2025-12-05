/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Thread-Safe Brightness Control API for v2.0
 *
 * IMPORTANT: Work queue only sends timer messages.
 * All I2C sensor reading and PWM changes happen in scanner_display.c main thread.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Enable/disable auto brightness periodic timer
// This only affects whether the sensor work queue sends read requests
void brightness_control_set_auto(bool enabled);

// Check if auto brightness is enabled
bool brightness_control_is_auto(void);

// Get I2C device for sensor access (main thread only!)
const struct device *brightness_control_get_i2c_dev(void);

// Check if sensor is available
bool brightness_control_sensor_available(void);

// Read light sensor value (main thread only!)
// Returns 0 on success, negative error code otherwise
int brightness_control_read_sensor(uint16_t *light_val);

// Map light value to brightness percentage
uint8_t brightness_control_map_light_to_brightness(uint32_t light_value);
