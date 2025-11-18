/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/**
 * Scanner update event - triggered when keyboard status changes
 *
 * This event is raised from work queue thread (safe) and handled by
 * listener in main thread (LVGL-safe).
 *
 * Thread Safety:
 * - Raised by: notify_work_handler() in system work queue
 * - Handled by: scanner_update_listener() in main thread
 * - LVGL operations: Only in listener (main thread) - SAFE
 */

enum zmk_status_scanner_event_type {
    ZMK_STATUS_SCANNER_EVENT_BATTERY_CHANGED = 0,
    ZMK_STATUS_SCANNER_EVENT_LAYER_CHANGED = 1,
    ZMK_STATUS_SCANNER_EVENT_CONNECTION_CHANGED = 2,
    ZMK_STATUS_SCANNER_EVENT_MODIFIER_CHANGED = 3,
    ZMK_STATUS_SCANNER_EVENT_WPM_CHANGED = 4,
    ZMK_STATUS_SCANNER_EVENT_NEW_DEVICE = 5,
    ZMK_STATUS_SCANNER_EVENT_DEVICE_TIMEOUT = 6,
};

struct zmk_scanner_update_event {
    enum zmk_status_scanner_event_type event;
    int keyboard_index;  // Index of keyboard that changed (-1 for all)
};

ZMK_EVENT_DECLARE(zmk_scanner_update_event);
