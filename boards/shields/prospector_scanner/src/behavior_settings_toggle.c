/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_settings_toggle

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>

#include "system_settings_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

// DEPRECATED: This behavior is deprecated due to dynamic widget allocation
// Settings toggle is now handled via swipe gestures in scanner_display.c
static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    LOG_WRN("⚠️  Settings toggle behavior is deprecated - use swipe gestures instead");
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    // Nothing to do on release
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_settings_toggle_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_settings_toggle_init(const struct device *dev) {
    LOG_INF("Settings toggle behavior initialized");
    return 0;
}

DEVICE_DT_INST_DEFINE(0, behavior_settings_toggle_init, NULL, NULL, NULL,
                      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                      &behavior_settings_toggle_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
