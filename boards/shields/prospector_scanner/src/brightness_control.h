/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// Set manual brightness (0-100%)
void brightness_control_set_manual(uint8_t brightness);

// Enable/disable auto brightness (only works if sensor is available)
void brightness_control_set_auto(bool enabled);

// Get current brightness value
uint8_t brightness_control_get_current(void);

// Check if auto brightness is currently enabled
bool brightness_control_is_auto(void);
