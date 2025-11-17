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
#include <zephyr/dt-bindings/input/input-event-codes.h>  // INPUT_KEY_DOWN, etc.
#include <zephyr/logging/log.h>
#include <lvgl.h>

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

// External reference to main screen (defined in scanner_display.c)
extern lv_obj_t *main_screen;

// Work queue for LVGL operations (must run in thread context, not ISR)
static struct k_work bg_red_work;
static struct k_work bg_black_work;

// Simple test: Change background color only (no overlay, no complex UI)
static void bg_red_work_handler(struct k_work *work) {
    LOG_INF("🔴 Setting background to RED (down swipe test)");
    if (main_screen) {
        lv_obj_set_style_bg_color(main_screen, lv_color_hex(0xFF0000), 0);
        LOG_INF("✅ Background changed to RED");
    } else {
        LOG_ERR("❌ main_screen is NULL!");
    }
}

static void bg_black_work_handler(struct k_work *work) {
    LOG_INF("⚫ Setting background to BLACK (up swipe test)");
    if (main_screen) {
        lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x000000), 0);
        LOG_INF("✅ Background changed to BLACK");
    } else {
        LOG_ERR("❌ main_screen is NULL!");
    }
}

/**
 * Input event callback for CST816S touch sensor
 *
 * This receives INPUT_BTN_TOUCH, INPUT_ABS_X, INPUT_ABS_Y events from Zephyr
 */
// External callback registration function that can be called from scanner_display.c
extern void touch_handler_late_register_callback(touch_event_callback_t callback);

static void touch_input_callback(struct input_event *evt) {
    LOG_INF("📥 INPUT EVENT: type=%d code=%d value=%d", evt->type, evt->code, evt->value);

    switch (evt->code) {
        case INPUT_KEY_DOWN:
            // CST816S hardware gesture: Swipe DOWN detected
            if (evt->value == 1) {  // Key press
                LOG_INF("⬇️ CST816S HARDWARE GESTURE: Swipe DOWN detected - submitting bg_red_work");
                k_work_submit(&bg_red_work);
            }
            break;

        case INPUT_KEY_UP:
            if (evt->value == 1) {
                LOG_INF("⬆️ CST816S HARDWARE GESTURE: Swipe UP detected - submitting bg_black_work");
                k_work_submit(&bg_black_work);
            }
            break;

        case INPUT_KEY_LEFT:
            if (evt->value == 1) {
                LOG_INF("⬅️ CST816S HARDWARE GESTURE: Swipe LEFT detected - ACTION DISABLED FOR DEBUG");
                // Future: implement swipe left action
            }
            break;

        case INPUT_KEY_RIGHT:
            if (evt->value == 1) {
                LOG_INF("➡️ CST816S HARDWARE GESTURE: Swipe RIGHT detected - ACTION DISABLED FOR DEBUG");
                // Future: implement swipe right action
            }
            break;

        case INPUT_ABS_X:
            // Store X coordinate
            current_x = (uint16_t)evt->value;
            x_updated = true;
            LOG_INF("📍 X: %d", current_x);
            break;

        case INPUT_ABS_Y:
            // Store Y coordinate
            current_y = (uint16_t)evt->value;
            y_updated = true;
            LOG_INF("📍 Y: %d", current_y);
            break;

        case INPUT_BTN_TOUCH:
            // Touch state changed
            touch_active = (evt->value != 0);
            LOG_INF("🔔 BTN_TOUCH event: value=%d, prev_active=%d, new_active=%d",
                    evt->value, prev_touch_active, touch_active);

            // Wait for coordinates to be updated
            if (!x_updated || !y_updated) {
                LOG_WRN("⚠️  Touch event before coordinates updated, using previous values");
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
                // Touch DOWN - record start position ONLY at touch start
                swipe_state.start_x = current_x;
                swipe_state.start_y = current_y;
                swipe_state.start_time = k_uptime_get();
                swipe_state.in_progress = true;

                LOG_INF("🖐️ Touch DOWN at (%d, %d)", current_x, current_y);

                // Reset coordinate update flags for next touch
                x_updated = false;
                y_updated = false;
            } else if (touch_active) {
                // Touch is being held (dragging) - just log current position (reduced logging)
                LOG_DBG("👆 Dragging at (%d, %d)", current_x, current_y);
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

                LOG_DBG("👆 Swipe: (%d,%d) → (%d,%d), raw dx=%d dy=%d → physical dx=%d dy=%d, in_progress=%d",
                        swipe_state.start_x, swipe_state.start_y, current_x, current_y,
                        raw_dx, raw_dy, dx, dy, swipe_state.in_progress);

                if (swipe_state.in_progress) {
                    // Check if movement is primarily vertical and exceeds threshold
                    if (abs_dy > abs_dx && abs_dy > SWIPE_THRESHOLD) {
                        if (dy > 0) {
                            // DOWN swipe detected - TEST: change bg to RED
                            LOG_INF("⬇️ DOWN SWIPE detected (physical dy=%d, threshold=%d) - submitting bg_red_work", dy, SWIPE_THRESHOLD);
                            k_work_submit(&bg_red_work);
                        } else {
                            // UP swipe detected - TEST: change bg to BLACK
                            LOG_INF("⬆️ UP SWIPE detected (physical dy=%d, threshold=%d) - submitting bg_black_work", dy, SWIPE_THRESHOLD);
                            k_work_submit(&bg_black_work);
                        }
                    } else {
                        LOG_INF("↔️ HORIZONTAL swipe: abs_dx=%d, abs_dy=%d (threshold=%d)",
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

            // Update previous state for next event
            prev_touch_active = touch_active;
            break;

        default:
            LOG_DBG("Unknown input event: type=%d, code=%d, value=%d", evt->type, evt->code, evt->value);
            break;
    }
}

// Input callback registration macro
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_input_callback);

// LVGL input device read callback
static void lvgl_input_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    data->point.x = current_x;
    data->point.y = current_y;
    data->state = touch_active ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int touch_handler_init(void) {
    const struct device *touch_dev = DEVICE_DT_GET(TOUCH_NODE);

    if (!device_is_ready(touch_dev)) {
        LOG_ERR("Touch sensor device not ready");
        return -ENODEV;
    }

    LOG_INF("Touch handler initialized: CST816S on I2C");
    LOG_INF("Touch panel size: 240x280 (Waveshare 1.69\" Round LCD)");

    // Initialize work queues for LVGL operations (must run in thread context)
    k_work_init(&bg_red_work, bg_red_work_handler);
    k_work_init(&bg_black_work, bg_black_work_handler);
    LOG_INF("✅ Work queues initialized (simple bg color test)");

    // Register LVGL input device for touch events
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_input_read;
    lvgl_indev = lv_indev_drv_register(&indev_drv);

    if (!lvgl_indev) {
        LOG_ERR("Failed to register LVGL input device");
        return -ENOMEM;
    }

    LOG_INF("✅ LVGL input device registered for touch events");

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
