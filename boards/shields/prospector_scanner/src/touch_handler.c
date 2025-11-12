/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "touch_handler.h"
#include "system_settings_widget.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

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

// External reference to settings widget (defined in scanner_display.c)
extern struct zmk_widget_system_settings system_settings_widget;

/**
 * Input event callback for CST816S touch sensor
 *
 * This receives INPUT_BTN_TOUCH, INPUT_ABS_X, INPUT_ABS_Y events from Zephyr
 */
// External callback registration function that can be called from scanner_display.c
extern void touch_handler_late_register_callback(touch_event_callback_t callback);

static void touch_input_callback(struct input_event *evt) {
    switch (evt->code) {
        case INPUT_ABS_X:
            // Store X coordinate
            current_x = (uint16_t)evt->value;
            x_updated = true;
            LOG_DBG("📍 X: %d", current_x);
            break;

        case INPUT_ABS_Y:
            // Store Y coordinate
            current_y = (uint16_t)evt->value;
            y_updated = true;
            LOG_DBG("📍 Y: %d", current_y);
            break;

        case INPUT_BTN_TOUCH:
            // Touch state changed
            touch_active = (evt->value != 0);

            // Wait for coordinates to be updated
            if (!x_updated || !y_updated) {
                LOG_WRN("⚠️  Touch event before coordinates updated, using previous values");
            }

            // Update last event with complete coordinates
            last_event.x = current_x;
            last_event.y = current_y;
            last_event.touched = touch_active;
            last_event.timestamp = k_uptime_get_32();

            if (touch_active) {
                // Touch DOWN - record start position
                swipe_state.start_x = current_x;
                swipe_state.start_y = current_y;
                swipe_state.start_time = k_uptime_get();
                swipe_state.in_progress = true;

                LOG_INF("🖐️ Touch DOWN at (%d, %d)", current_x, current_y);

                // Reset coordinate update flags for next touch
                x_updated = false;
                y_updated = false;
            } else {
                // Touch UP - check for swipe gesture
                // NOTE: Display orientation vs touch panel coordinate system mismatch
                // Physical vertical swipe → coordinate X axis changes (90° rotation)
                // Physical horizontal swipe → coordinate Y axis changes
                int16_t raw_dx = current_x - swipe_state.start_x;
                int16_t raw_dy = current_y - swipe_state.start_y;

                // COORDINATE TRANSFORM: Swap X/Y axes for 90° rotation
                // Physical vertical movement → coordinate X movement (inverted)
                // Physical horizontal movement → coordinate Y movement
                int16_t dx = -raw_dy;  // Physical horizontal = coordinate Y (inverted)
                int16_t dy = -raw_dx;  // Physical vertical = coordinate X (inverted)

                int16_t abs_dx = (dx < 0) ? -dx : dx;
                int16_t abs_dy = (dy < 0) ? -dy : dy;

                LOG_INF("👆 Swipe: (%d,%d) → (%d,%d), raw dx=%d dy=%d → physical dx=%d dy=%d",
                        swipe_state.start_x, swipe_state.start_y, current_x, current_y,
                        raw_dx, raw_dy, dx, dy);

                if (swipe_state.in_progress) {
                    // Check if movement is primarily vertical and exceeds threshold
                    if (abs_dy > abs_dx && abs_dy > SWIPE_THRESHOLD) {
                        if (dy > 0) {
                            // DOWN swipe detected
                            LOG_INF("⬇️ DOWN SWIPE detected (physical dy=%d, threshold=%d)", dy, SWIPE_THRESHOLD);

                            // Toggle settings screen
                            if (!system_settings_widget.obj) {
                                LOG_ERR("Settings widget not initialized");
                            } else {
                                bool is_visible = !lv_obj_has_flag(system_settings_widget.obj,
                                                                   LV_OBJ_FLAG_HIDDEN);
                                if (is_visible) {
                                    zmk_widget_system_settings_hide(&system_settings_widget);
                                    LOG_INF("✅ Settings screen HIDDEN");
                                } else {
                                    zmk_widget_system_settings_show(&system_settings_widget);
                                    LOG_INF("✅ Settings screen SHOWN");
                                }
                            }
                        } else {
                            // UP swipe detected
                            LOG_INF("⬆️ UP SWIPE detected (physical dy=%d, threshold=%d)", dy, SWIPE_THRESHOLD);
                        }
                    } else {
                        LOG_INF("❌ Horizontal swipe: abs_dx=%d, abs_dy=%d (threshold=%d)",
                                abs_dx, abs_dy, SWIPE_THRESHOLD);
                    }

                    swipe_state.in_progress = false;
                }

                // Reset coordinate update flags
                x_updated = false;
                y_updated = false;
            }

            // Call registered callback if available (for future gesture implementation)
            if (registered_callback) {
                registered_callback(&last_event);
            }
            break;

        default:
            break;
    }
}

// Input callback registration macro
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_input_callback);

int touch_handler_init(void) {
    const struct device *touch_dev = DEVICE_DT_GET(TOUCH_NODE);

    if (!device_is_ready(touch_dev)) {
        LOG_ERR("Touch sensor device not ready");
        return -ENODEV;
    }

    LOG_INF("Touch handler initialized: CST816S on I2C");
    LOG_INF("Touch panel size: 240x280 (Waveshare 1.69\" Round LCD)");

    return 0;
}

int touch_handler_register_callback(touch_event_callback_t callback) {
    if (!callback) {
        LOG_ERR("❌ Callback is NULL!");
        return -EINVAL;
    }

    registered_callback = callback;
    LOG_INF("✅ Touch callback registered successfully: callback=%p", (void*)callback);

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
