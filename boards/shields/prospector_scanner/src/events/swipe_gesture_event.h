/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

// Swipe gesture directions
enum swipe_direction {
    SWIPE_DIRECTION_NONE = -1,  /* No pending swipe */
    SWIPE_DIRECTION_UP = 0,
    SWIPE_DIRECTION_DOWN = 1,
    SWIPE_DIRECTION_LEFT = 2,
    SWIPE_DIRECTION_RIGHT = 3,
};

// Swipe gesture event - raised by touch handler, processed by display thread
struct zmk_swipe_gesture_event {
    enum swipe_direction direction;
};

ZMK_EVENT_DECLARE(zmk_swipe_gesture_event);
