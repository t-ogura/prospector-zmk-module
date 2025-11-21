/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Thread-Safe Brightness Control API for v2.0
 *
 * IMPORTANT: These functions ONLY control the sensor reading work queue.
 * All actual PWM brightness changes happen in scanner_display.c via messages.
 */

#pragma once

#include <stdbool.h>

// Enable/disable auto brightness sensor reading
// This only affects whether the sensor work queue is active
// Does NOT directly change brightness - sends messages instead
void brightness_control_set_auto(bool enabled);

// Check if auto brightness sensor reading is enabled
bool brightness_control_is_auto(void);
