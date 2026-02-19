/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Prospector Display Layouts - Scanner Mode Manager
 *
 * Manages switching between Operator and Radii layouts.
 * Use prospector_layouts_next()/prev() to switch layouts.
 */

#include "prospector_layouts.h"
#include "operator_layout.h"
#include "radii_layout.h"
#include "field_layout.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(prospector_layouts, CONFIG_ZMK_LOG_LEVEL);

/* Available layouts: Operator and Radii */
static prospector_layout_t current_layout = PROSPECTOR_LAYOUT_OPERATOR;
static lv_obj_t *parent_obj = NULL;
static bool initialized = false;

/* Cached data for layout switching */
static struct prospector_keyboard_data cached_data = {0};

/* Forward declarations */
static void destroy_current_layout(void);
static void create_current_layout(void);
static void update_current_layout(void);

void prospector_layouts_init(lv_obj_t *parent) {
    if (initialized) {
        LOG_WRN("Prospector layouts already initialized");
        return;
    }

    parent_obj = parent;

    /* Create initial layout (Operator) */
    create_current_layout();

    initialized = true;
    LOG_INF("Prospector layouts initialized (%s)", prospector_layouts_get_name(current_layout));
}

void prospector_layouts_destroy(void) {
    if (!initialized) {
        return;
    }

    destroy_current_layout();

    parent_obj = NULL;
    initialized = false;
    LOG_INF("Prospector layouts destroyed");
}

void prospector_layouts_set_style(prospector_layout_t layout) {
    if (!initialized) return;

    /* Support Field, Operator, and Radii */
    if (layout != PROSPECTOR_LAYOUT_FIELD &&
        layout != PROSPECTOR_LAYOUT_OPERATOR &&
        layout != PROSPECTOR_LAYOUT_RADII) {
        layout = PROSPECTOR_LAYOUT_OPERATOR;
    }

    if (layout == current_layout) {
        return;
    }

    /* Switch layout */
    destroy_current_layout();
    current_layout = layout;
    create_current_layout();
    update_current_layout();

    LOG_INF("Layout switched to %s", prospector_layouts_get_name(current_layout));
}

prospector_layout_t prospector_layouts_get_style(void) {
    return current_layout;
}

void prospector_layouts_next(void) {
    if (!initialized) return;

    /* Cycle: Field -> Operator -> Radii -> Field */
    prospector_layout_t next;
    switch (current_layout) {
    case PROSPECTOR_LAYOUT_FIELD:
        next = PROSPECTOR_LAYOUT_OPERATOR;
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        next = PROSPECTOR_LAYOUT_RADII;
        break;
    case PROSPECTOR_LAYOUT_RADII:
    default:
        next = PROSPECTOR_LAYOUT_FIELD;
        break;
    }
    prospector_layouts_set_style(next);
}

void prospector_layouts_prev(void) {
    if (!initialized) return;

    /* Cycle reverse: Field <- Operator <- Radii <- Field */
    prospector_layout_t prev;
    switch (current_layout) {
    case PROSPECTOR_LAYOUT_FIELD:
        prev = PROSPECTOR_LAYOUT_RADII;
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        prev = PROSPECTOR_LAYOUT_FIELD;
        break;
    case PROSPECTOR_LAYOUT_RADII:
    default:
        prev = PROSPECTOR_LAYOUT_OPERATOR;
        break;
    }
    prospector_layouts_set_style(prev);
}

void prospector_layouts_update(const struct prospector_keyboard_data *data) {
    if (!initialized || data == NULL) {
        return;
    }

    /* Cache data for layout switching */
    cached_data = *data;

    update_current_layout();
}

void prospector_layouts_cycle_palette(void) {
    if (!initialized) return;

    switch (current_layout) {
    case PROSPECTOR_LAYOUT_FIELD:
        field_layout_cycle_palette();
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        operator_layout_cycle_palette();
        break;
    case PROSPECTOR_LAYOUT_RADII:
        radii_layout_cycle_palette();
        break;
    default:
        break;
    }
}

const char *prospector_layouts_get_name(prospector_layout_t layout) {
    switch (layout) {
    case PROSPECTOR_LAYOUT_CLASSIC:
        return "Classic";
    case PROSPECTOR_LAYOUT_FIELD:
        return "Field";
    case PROSPECTOR_LAYOUT_OPERATOR:
        return "Operator";
    case PROSPECTOR_LAYOUT_RADII:
        return "Radii";
    default:
        return "Unknown";
    }
}

/* ========== Internal Functions ========== */

static void destroy_current_layout(void) {
    switch (current_layout) {
    case PROSPECTOR_LAYOUT_FIELD:
        field_layout_destroy();
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        operator_layout_destroy();
        break;
    case PROSPECTOR_LAYOUT_RADII:
        radii_layout_destroy();
        break;
    default:
        break;
    }
}

static void create_current_layout(void) {
    if (!parent_obj) return;

    switch (current_layout) {
    case PROSPECTOR_LAYOUT_FIELD:
        field_layout_create(parent_obj);
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        operator_layout_create(parent_obj);
        break;
    case PROSPECTOR_LAYOUT_RADII:
        radii_layout_create(parent_obj);
        break;
    default:
        /* Fallback to Operator */
        current_layout = PROSPECTOR_LAYOUT_OPERATOR;
        operator_layout_create(parent_obj);
        break;
    }
}

static void update_current_layout(void) {
    /* Convert cached data to layout-specific update calls */
    uint8_t active_layer = cached_data.active_layer;
    const char *layer_name = cached_data.current_layer_name[0]
                             ? cached_data.current_layer_name : "BASE";

    uint8_t battery_level = cached_data.battery_level;
    bool battery_connected = cached_data.has_dynamic_data && battery_level > 0;

    uint8_t peripheral_battery = cached_data.peripheral_battery[0];
    bool peripheral_connected = cached_data.has_dynamic_data && peripheral_battery > 0;

    uint8_t wpm = cached_data.wpm_value;
    uint8_t modifier_flags = cached_data.modifier_flags;

    bool usb_connected = cached_data.usb_connected;
    uint8_t ble_profile = cached_data.profile_slot;
    bool ble_connected = cached_data.ble_connected;
    bool ble_bonded = cached_data.ble_bonded;

    switch (current_layout) {
    case PROSPECTOR_LAYOUT_FIELD:
        field_layout_update(
            active_layer, layer_name,
            battery_level, battery_connected,
            peripheral_battery, peripheral_connected,
            wpm, modifier_flags,
            usb_connected, ble_profile,
            ble_connected, ble_bonded
        );
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        operator_layout_update(
            active_layer, layer_name,
            battery_level, battery_connected,
            peripheral_battery, peripheral_connected,
            wpm, modifier_flags,
            usb_connected, ble_profile,
            ble_connected, ble_bonded
        );
        break;
    case PROSPECTOR_LAYOUT_RADII:
        radii_layout_update(
            active_layer, layer_name,
            battery_level, battery_connected,
            peripheral_battery, peripheral_connected,
            modifier_flags, usb_connected, ble_profile
        );
        break;
    default:
        break;
    }
}
