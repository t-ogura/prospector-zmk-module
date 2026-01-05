/**
 * Swipe Gesture Event - Thread-safe swipe notification
 */

#pragma once

#include <zmk/event_manager.h>

enum swipe_direction {
    SWIPE_DIRECTION_NONE = 0,
    SWIPE_DIRECTION_UP,
    SWIPE_DIRECTION_DOWN,
    SWIPE_DIRECTION_LEFT,
    SWIPE_DIRECTION_RIGHT,
};

struct zmk_swipe_gesture_event {
    enum swipe_direction direction;
};

ZMK_EVENT_DECLARE(zmk_swipe_gesture_event);
