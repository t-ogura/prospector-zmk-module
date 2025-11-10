/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "touch_handler.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TOUCH_NODE DT_NODELABEL(touch_sensor)

#if !DT_NODE_EXISTS(TOUCH_NODE)
#error "Touch sensor device tree node not found"
#endif

// Touch event state
static struct touch_event_data last_event = {0};
static touch_event_callback_t registered_callback = NULL;
static bool touch_active = false;

// Current touch coordinates (accumulated from INPUT_ABS_X/Y events)
static uint16_t current_x = 0;
static uint16_t current_y = 0;

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
            LOG_INF("📍 X: %d", current_x);
            break;

        case INPUT_ABS_Y:
            // Store Y coordinate
            current_y = (uint16_t)evt->value;
            LOG_INF("📍 Y: %d", current_y);
            break;

        case INPUT_BTN_TOUCH:
            // Touch state changed
            touch_active = (evt->value != 0);

            // Update last event with complete coordinates
            last_event.x = current_x;
            last_event.y = current_y;
            last_event.touched = touch_active;
            last_event.timestamp = k_uptime_get_32();

            LOG_INF("🖐️ Touch %s at (%d, %d)",
                    touch_active ? "DOWN" : "UP",
                    last_event.x, last_event.y);

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
