/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "touch_handler.h"
#include "events/swipe_gesture_event.h"
// Message queue removed - using ZMK event system for thread-safe architecture

/* Weak function - overridden by display_settings_widget.c when included */
__attribute__((weak)) bool display_settings_is_interacting(void) {
    return false;  /* Default: never blocking */
}

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>  // INPUT_KEY_DOWN, etc.
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TOUCH_NODE DT_NODELABEL(touch_sensor)

#if !DT_NODE_EXISTS(TOUCH_NODE)
#error "Touch sensor device tree node not found"
#endif

// Swipe gesture detection settings
#define SWIPE_THRESHOLD 30  // Minimum pixels for valid swipe (adjusted for 180° rotated display)

// Touch event state
static struct touch_event_data last_event = {0};
static touch_event_callback_t registered_callback = NULL;
static bool touch_active = false;
static bool prev_touch_active = false;  // Track previous state to detect touch start

// LVGL input device
static lv_indev_t *lvgl_indev = NULL;

// Current touch coordinates (accumulated from INPUT_ABS_X/Y events)
static uint16_t current_x = 0;
static uint16_t current_y = 0;
static bool x_updated = false;
static bool y_updated = false;

// Swipe gesture state
static struct {
    int16_t start_x;
    int16_t start_y;
    int64_t start_time;
    bool in_progress;
} swipe_state = {0};

// Cooldown to prevent rapid-fire swipe events (prevents freeze)
#define SWIPE_COOLDOWN_MS 400
static int64_t last_swipe_time = 0;

// Flag to prevent duplicate events (hardware gesture + software detection)
static bool swipe_already_raised = false;

// Helper function to raise swipe gesture event (thread-safe)
// Uses ZMK event system - listener runs in main thread, safe for LVGL
static void raise_swipe_event(enum swipe_direction direction) {
    const char *dir_name[] = {"UP", "DOWN", "LEFT", "RIGHT"};

    // Cooldown check - prevent rapid consecutive swipes that cause freeze
    int64_t now = k_uptime_get();
    if ((now - last_swipe_time) < SWIPE_COOLDOWN_MS) {
        LOG_DBG("Swipe blocked - cooldown active (%lld ms remaining)",
                SWIPE_COOLDOWN_MS - (now - last_swipe_time));
        return;
    }

    // Block swipe during UI interaction (slider drag, etc.)
    if (display_settings_is_interacting()) {
        LOG_DBG("Swipe blocked - UI interaction in progress");
        return;
    }

    last_swipe_time = now;
    LOG_DBG("Raising ZMK swipe event: %s", dir_name[direction]);

    // Use ZMK event system - thread-safe, listener runs in main thread
    // No message queue needed - ZMK event manager handles synchronization
    raise_zmk_swipe_gesture_event(
        (struct zmk_swipe_gesture_event){.direction = direction}
    );
}

/**
 * Input event callback for CST816S touch sensor
 *
 * This receives INPUT_BTN_TOUCH, INPUT_ABS_X, INPUT_ABS_Y events from Zephyr
 */
// External callback registration function that can be called from scanner_display.c
extern void touch_handler_late_register_callback(touch_event_callback_t callback);

static void touch_input_callback(struct input_event *evt, void *user_data) {
    ARG_UNUSED(user_data);
    // Reduced logging - only log swipe gestures, not every coordinate update
    LOG_DBG("📥 INPUT EVENT: type=%d code=%d value=%d", evt->type, evt->code, evt->value);

    switch (evt->code) {
        case INPUT_KEY_DOWN:
            // CST816S hardware gesture: Swipe DOWN detected
            if (evt->value == 1) {
                LOG_DBG("HW GESTURE: Swipe DOWN");
                swipe_already_raised = true;
                raise_swipe_event(SWIPE_DIRECTION_DOWN);
            }
            break;

        case INPUT_KEY_UP:
            if (evt->value == 1) {
                LOG_DBG("HW GESTURE: Swipe UP");
                swipe_already_raised = true;
                raise_swipe_event(SWIPE_DIRECTION_UP);
            }
            break;

        case INPUT_KEY_LEFT:
            if (evt->value == 1) {
                // Direct mapping - no swap needed with corrected coordinate transform
                LOG_DBG("HW GESTURE: Swipe LEFT");
                swipe_already_raised = true;
                raise_swipe_event(SWIPE_DIRECTION_LEFT);
            }
            break;

        case INPUT_KEY_RIGHT:
            if (evt->value == 1) {
                // Direct mapping - no swap needed with corrected coordinate transform
                LOG_DBG("HW GESTURE: Swipe RIGHT");
                swipe_already_raised = true;
                raise_swipe_event(SWIPE_DIRECTION_RIGHT);
            }
            break;

        case INPUT_ABS_X:
            // Store X coordinate
            current_x = (uint16_t)evt->value;
            x_updated = true;
            // No logging for coordinates - too verbose
            break;

        case INPUT_ABS_Y:
            // Store Y coordinate
            current_y = (uint16_t)evt->value;
            y_updated = true;
            // No logging for coordinates - too verbose
            break;

        case INPUT_BTN_TOUCH:
            // Touch state changed
            touch_active = (evt->value != 0);
            LOG_DBG("🔔 BTN_TOUCH event: value=%d, prev_active=%d, new_active=%d",
                    evt->value, prev_touch_active, touch_active);

            // Wait for coordinates to be updated
            if (!x_updated || !y_updated) {
                LOG_DBG("Touch event before coordinates updated, using previous values");
            }

            // Update last event with complete coordinates
            last_event.x = current_x;
            last_event.y = current_y;
            last_event.touched = touch_active;
            last_event.timestamp = k_uptime_get_32();

            // Detect touch start (false → true transition)
            bool touch_started = touch_active && !prev_touch_active;

            LOG_DBG("🔍 Touch state: touch_active=%d, prev_touch_active=%d, touch_started=%d",
                    touch_active, prev_touch_active, touch_started);

            if (touch_started) {
                // Touch DOWN - record start position and clear duplicate flag
                swipe_state.start_x = current_x;
                swipe_state.start_y = current_y;
                swipe_state.start_time = k_uptime_get();
                swipe_state.in_progress = true;
                swipe_already_raised = false;  // Clear for new touch sequence

                LOG_DBG("Touch DOWN at (%d, %d)", current_x, current_y);

                // Reset coordinate update flags for next touch
                x_updated = false;
                y_updated = false;
            } else if (touch_active) {
                // Touch is being held (dragging) - just log current position (reduced logging)
                LOG_DBG("Dragging at (%d, %d)", current_x, current_y);
            } else {
                // Touch UP - Software swipe detection (fallback if no hardware gesture)
                if (!swipe_already_raised && swipe_state.in_progress) {
                    int16_t raw_dx = current_x - swipe_state.start_x;
                    int16_t raw_dy = current_y - swipe_state.start_y;

                    // COORDINATE TRANSFORM for rotated display
                    // Touch Y → Display X (direct), Touch X → Display Y (inverted)
                    int16_t dx = raw_dy;   // X: direct mapping
                    int16_t dy = -raw_dx;  // Y: inverted

                    int16_t abs_dx = (dx < 0) ? -dx : dx;
                    int16_t abs_dy = (dy < 0) ? -dy : dy;

                    if (abs_dy > abs_dx && abs_dy > SWIPE_THRESHOLD) {
                        LOG_DBG("SW SWIPE: %s (dy=%d)", dy > 0 ? "DOWN" : "UP", dy);
                        raise_swipe_event(dy > 0 ? SWIPE_DIRECTION_DOWN : SWIPE_DIRECTION_UP);
                    } else if (abs_dx > abs_dy && abs_dx > SWIPE_THRESHOLD) {
                        LOG_DBG("SW SWIPE: %s (dx=%d)", dx > 0 ? "RIGHT" : "LEFT", dx);
                        raise_swipe_event(dx > 0 ? SWIPE_DIRECTION_RIGHT : SWIPE_DIRECTION_LEFT);
                    }
                }

                swipe_state.in_progress = false;
                swipe_already_raised = false;

                // Reset coordinate update flags
                x_updated = false;
                y_updated = false;
            }

            // Call registered callback if available (for future gesture implementation)
            if (registered_callback) {
                registered_callback(&last_event);
            }

            // Update previous state for next event
            prev_touch_active = touch_active;
            break;

        default:
            LOG_DBG("Unknown input event: type=%d, code=%d, value=%d", evt->type, evt->code, evt->value);
            break;
    }
}

// Input callback registration macro (Zephyr 4.x requires 3 args: dev, callback, user_data)
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_input_callback, NULL);

// LVGL input device read callback (LVGL 9 API)
// COORDINATE TRANSFORM:
// - Touch panel physical: 240 x 280 (portrait)
// - Display logical: 280 x 240 (landscape, rotated 90° via ST7789V mdac)
// - Touch Y (0-279) → Display X (0-279) - direct mapping, NO inversion
// - Touch X (0-239) → Display Y (239-0) - inverted
static void lvgl_input_read(lv_indev_t *indev, lv_indev_data_t *data) {
    ARG_UNUSED(indev);

    // Transform coordinates: swap X/Y axes
    // Touch panel Y axis maps to display X axis (direct, no inversion)
    // Touch panel X axis maps to display Y axis (inverted)
    int32_t logical_x = current_y;           // Direct mapping
    int32_t logical_y = 239 - current_x;     // Inverted

    // Clamp to valid range
    if (logical_x < 0) logical_x = 0;
    if (logical_x > 279) logical_x = 279;
    if (logical_y < 0) logical_y = 0;
    if (logical_y > 239) logical_y = 239;

    data->point.x = logical_x;
    data->point.y = logical_y;
    data->state = touch_active ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    // Debug: Log when LVGL reads touch state (reduced frequency)
    static uint32_t last_log_time = 0;
    uint32_t now = k_uptime_get_32();
    if (now - last_log_time > 500 || touch_active) {
        LOG_DBG("LVGL read: raw(%d,%d) -> logical(%d,%d) state=%s",
                current_x, current_y,
                (int)data->point.x, (int)data->point.y,
                touch_active ? "PRESSED" : "RELEASED");
        last_log_time = now;
    }
}

int touch_handler_init(void) {
    const struct device *touch_dev = DEVICE_DT_GET(TOUCH_NODE);

    if (!device_is_ready(touch_dev)) {
        LOG_ERR("Touch sensor device not ready");
        return -ENODEV;
    }

    LOG_INF("Touch handler initialized: CST816S (240x280)");

    return 0;
}

int touch_handler_register_lvgl_indev(void) {
    if (lvgl_indev) {
        LOG_WRN("LVGL input device already registered");
        return 0;
    }

    // Register LVGL input device for touch events (LVGL 9 API)
    lvgl_indev = lv_indev_create();
    if (!lvgl_indev) {
        LOG_ERR("Failed to create LVGL input device");
        return -ENOMEM;
    }

    lv_indev_set_type(lvgl_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvgl_indev, lvgl_input_read);

    LOG_DBG("LVGL input device registered for touch events");

    return 0;
}

int touch_handler_register_callback(touch_event_callback_t callback) {
    if (!callback) {
        LOG_ERR("Callback is NULL!");
        return -EINVAL;
    }

    registered_callback = callback;
    LOG_DBG("Touch callback registered: callback=%p", (void*)callback);

    return 0;
}

int touch_handler_get_last_event(struct touch_event_data *event) {
    if (!event) {
        return -EINVAL;
    }

    if (last_event.timestamp == 0) {
        return -ENODATA;
    }

    *event = last_event;
    return 0;
}
