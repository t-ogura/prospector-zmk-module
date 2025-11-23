/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "display_settings_widget.h"
#include "brightness_control.h"
#include "scanner_message.h"  // For brightness messages
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Global flag to indicate UI interaction in progress (prevents swipe)
static bool ui_interaction_active = false;

// Global settings that persist even when widget is destroyed
static bool g_auto_brightness_enabled = false;
static uint8_t g_manual_brightness = 65;
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
static bool g_battery_widget_visible = true;
#else
static bool g_battery_widget_visible = false;  // Default OFF when battery support disabled
#endif
static uint8_t g_max_layers = 7;

// Accessor functions for scanner_display.c
bool display_settings_get_battery_visible_global(void) {
    return g_battery_widget_visible;
}

uint8_t display_settings_get_max_layers_global(void) {
    return g_max_layers;
}

bool display_settings_is_interacting(void) {
    return ui_interaction_active;
}

// ========== Slider Drag Event Handler ==========

static void slider_drag_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        ui_interaction_active = true;
        LOG_DBG("🎚️ Slider drag started");
    } else if (code == LV_EVENT_RELEASED) {
        ui_interaction_active = false;
        LOG_DBG("🎚️ Slider drag ended");
    }
}

// ========== Event Handlers ==========

static void auto_brightness_sw_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    struct zmk_widget_display_settings *widget = lv_event_get_user_data(e);

    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    widget->auto_brightness_enabled = checked;
    g_auto_brightness_enabled = checked;  // Update global setting

    // CRITICAL: Sync with scanner_display.c auto_brightness_enabled via message
    // This ensures manual brightness slider works correctly
    scanner_msg_send_brightness_set_auto(checked);

    // Apply to brightness control system (sensor timer control)
    brightness_control_set_auto(checked);

    // Enable/disable manual slider based on auto state
    if (checked) {
        lv_obj_add_state(widget->brightness_slider, LV_STATE_DISABLED);
        lv_obj_set_style_opa(widget->brightness_slider, LV_OPA_50, 0);
    } else {
        lv_obj_clear_state(widget->brightness_slider, LV_STATE_DISABLED);
        lv_obj_set_style_opa(widget->brightness_slider, LV_OPA_COVER, 0);
        // Apply current manual brightness via message
        scanner_msg_send_brightness_set_target(widget->manual_brightness);
    }
}

static void brightness_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    struct zmk_widget_display_settings *widget = lv_event_get_user_data(e);

    int value = lv_slider_get_value(slider);
    widget->manual_brightness = (uint8_t)value;
    g_manual_brightness = (uint8_t)value;  // Update global setting

    // Update value label
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", value);
    lv_label_set_text(widget->brightness_value, buf);

    // Apply brightness immediately via message
    scanner_msg_send_brightness_set_target((uint8_t)value);
}

static void battery_sw_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    struct zmk_widget_display_settings *widget = lv_event_get_user_data(e);

    bool visible = lv_obj_has_state(sw, LV_STATE_CHECKED);
    widget->battery_widget_visible = visible;
    g_battery_widget_visible = visible;  // Update global setting
    LOG_INF("🔋 Battery widget: %s", visible ? "visible" : "hidden");
}

static void layer_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);
    struct zmk_widget_display_settings *widget = lv_event_get_user_data(e);

    int value = lv_slider_get_value(slider);
    widget->max_layers = (uint8_t)value;
    g_max_layers = (uint8_t)value;  // Update global setting

    // Update value label
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    lv_label_set_text(widget->layer_value, buf);

    LOG_DBG("📚 Max layers: %d", value);
}

// ========== Helper Functions ==========

static lv_obj_t *create_section_label(lv_obj_t *parent, const char *text, int y_offset) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);  // Brighter white
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);   // Larger font
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 15, y_offset);
    return label;
}

static lv_obj_t *create_switch(lv_obj_t *parent, int x_offset, int y_offset, bool initial_state) {
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 50, 28);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, x_offset, y_offset);

    if (initial_state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    // iPhone-style toggle switch
    // Background (pill shape)
    lv_obj_set_style_radius(sw, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3A3C), LV_PART_MAIN);  // iOS dark gray
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);

    // Indicator (colored background when ON)
    lv_obj_set_style_radius(sw, 14, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x34C759), LV_PART_INDICATOR | LV_STATE_CHECKED);  // iOS green
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3A3C), LV_PART_INDICATOR);  // iOS dark gray
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR);

    // Knob (round white circle)
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -2, LV_PART_KNOB);  // Smaller padding for tighter fit

    // Remove border
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_INDICATOR);

    return sw;
}

static lv_obj_t *create_slider(lv_obj_t *parent, int y_offset, int min, int max, int initial) {
    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_size(slider, 140, 6);  // Thin track, modern look
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 15, y_offset + 8);  // Center vertically with knob
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);

    // Extend click area for better touch response at edges
    lv_obj_set_ext_click_area(slider, 20);  // 20px extra touch area on all sides

    // Modern iOS-style slider
    // Track (thin rounded bar)
    lv_obj_set_style_radius(slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3A3A3C), LV_PART_MAIN);  // iOS dark gray
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);

    // Filled portion (indicator)
    lv_obj_set_style_radius(slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x007AFF), LV_PART_INDICATOR);  // iOS blue
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);

    // Round knob (circle thumb)
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);  // Large round knob
    lv_obj_set_style_shadow_width(slider, 4, LV_PART_KNOB);  // Subtle shadow
    lv_obj_set_style_shadow_color(slider, lv_color_hex(0x000000), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);

    // Remove border
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);

    return slider;
}

// ========== Widget Initialization ==========

static int zmk_widget_display_settings_init(struct zmk_widget_display_settings *widget, lv_obj_t *parent) {
    LOG_INF("⚙️ Display settings widget init START");

    if (!parent) {
        LOG_ERR("❌ Parent is NULL!");
        return -EINVAL;
    }

    widget->parent = parent;

    // Load values from global settings (persist across widget recreation)
    widget->auto_brightness_enabled = g_auto_brightness_enabled;
    widget->manual_brightness = g_manual_brightness;
    widget->battery_widget_visible = g_battery_widget_visible;
    widget->max_layers = g_max_layers;

    // Create full-screen container
    widget->obj = lv_obj_create(parent);
    if (!widget->obj) {
        LOG_ERR("❌ Failed to create container!");
        return -ENOMEM;
    }

    // Container styling
    lv_obj_set_size(widget->obj, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(widget->obj, 0, 0);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(widget->obj, 0, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);

    // Title
    widget->title_label = lv_label_create(widget->obj);
    lv_label_set_text(widget->title_label, "Display Settings");
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(widget->title_label, &lv_font_montserrat_18, 0);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 15);

    int y_pos = 50;

    // ========== Brightness Section ==========
    widget->brightness_label = create_section_label(widget->obj, "Brightness", y_pos);

    // Auto brightness toggle row
    lv_obj_t *auto_label = lv_label_create(widget->obj);
    lv_label_set_text(auto_label, "Auto");
    lv_obj_set_style_text_color(auto_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(auto_label, &lv_font_montserrat_12, 0);
    lv_obj_align(auto_label, LV_ALIGN_TOP_RIGHT, -70, y_pos + 4);

    widget->auto_brightness_sw = create_switch(widget->obj, -15, y_pos - 1, widget->auto_brightness_enabled);

#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
    // Sensor enabled: toggle is functional
    lv_obj_add_event_cb(widget->auto_brightness_sw, auto_brightness_sw_event_cb, LV_EVENT_VALUE_CHANGED, widget);
#else
    // Sensor disabled: toggle is locked OFF with red warning
    lv_obj_add_state(widget->auto_brightness_sw, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(widget->auto_brightness_sw, LV_OPA_50, LV_PART_MAIN);

    // Red warning label - positioned under Auto toggle for clarity
    lv_obj_t *warning_label = lv_label_create(widget->obj);
    lv_label_set_text(warning_label, "Disabled");
    lv_obj_set_style_text_color(warning_label, lv_color_hex(0xFF3B30), 0);  // iOS red
    lv_obj_set_style_text_font(warning_label, &lv_font_montserrat_12, 0);
    lv_obj_align(warning_label, LV_ALIGN_TOP_RIGHT, -15, y_pos + 22);  // Right-aligned under toggle
#endif

    y_pos += 35;

    // Brightness slider
    widget->brightness_slider = create_slider(widget->obj, y_pos, 10, 100, widget->manual_brightness);
    lv_obj_add_event_cb(widget->brightness_slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, widget);
    lv_obj_add_event_cb(widget->brightness_slider, slider_drag_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(widget->brightness_slider, slider_drag_event_cb, LV_EVENT_RELEASED, NULL);

    // Brightness value label
    widget->brightness_value = lv_label_create(widget->obj);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", widget->manual_brightness);
    lv_label_set_text(widget->brightness_value, buf);
    lv_obj_set_style_text_color(widget->brightness_value, lv_color_hex(0x007AFF), 0);  // iOS blue
    lv_obj_set_style_text_font(widget->brightness_value, &lv_font_montserrat_16, 0);   // Larger font
    lv_obj_align(widget->brightness_value, LV_ALIGN_TOP_RIGHT, -15, y_pos);

    y_pos += 40;

    // ========== Scanner Battery Widget Section ==========
    widget->battery_label = create_section_label(widget->obj, "Battery", y_pos);

    widget->battery_sw = create_switch(widget->obj, -15, y_pos - 3, widget->battery_widget_visible);

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    // Battery support enabled: toggle is functional
    lv_obj_add_event_cb(widget->battery_sw, battery_sw_event_cb, LV_EVENT_VALUE_CHANGED, widget);
#else
    // Battery support disabled: toggle is locked OFF with red warning
    lv_obj_clear_state(widget->battery_sw, LV_STATE_CHECKED);  // Force OFF
    lv_obj_add_state(widget->battery_sw, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(widget->battery_sw, LV_OPA_50, LV_PART_MAIN);

    // Red warning label - positioned under toggle
    lv_obj_t *battery_warning = lv_label_create(widget->obj);
    lv_label_set_text(battery_warning, "Disabled");
    lv_obj_set_style_text_color(battery_warning, lv_color_hex(0xFF3B30), 0);  // iOS red
    lv_obj_set_style_text_font(battery_warning, &lv_font_montserrat_12, 0);
    lv_obj_align(battery_warning, LV_ALIGN_TOP_RIGHT, -15, y_pos + 22);
#endif

    y_pos += 40;

    // ========== Max Layers Section ==========
    widget->layer_label = create_section_label(widget->obj, "Max Layers", y_pos);

    y_pos += 25;

    // Layer slider (4-10)
    widget->layer_slider = create_slider(widget->obj, y_pos, 4, 10, widget->max_layers);
    lv_obj_add_event_cb(widget->layer_slider, layer_slider_event_cb, LV_EVENT_VALUE_CHANGED, widget);
    lv_obj_add_event_cb(widget->layer_slider, slider_drag_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(widget->layer_slider, slider_drag_event_cb, LV_EVENT_RELEASED, NULL);

    // Layer value label
    widget->layer_value = lv_label_create(widget->obj);
    snprintf(buf, sizeof(buf), "%d", widget->max_layers);
    lv_label_set_text(widget->layer_value, buf);
    lv_obj_set_style_text_color(widget->layer_value, lv_color_hex(0x007AFF), 0);  // iOS blue
    lv_obj_set_style_text_font(widget->layer_value, &lv_font_montserrat_16, 0);   // Larger font
    lv_obj_align(widget->layer_value, LV_ALIGN_TOP_RIGHT, -15, y_pos);

    // Initially hidden
    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);

    LOG_INF("✅ Display settings widget initialized");
    return 0;
}

// ========== Dynamic Allocation Functions ==========

struct zmk_widget_display_settings *zmk_widget_display_settings_create(lv_obj_t *parent) {
    LOG_DBG("Creating display settings widget");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    struct zmk_widget_display_settings *widget =
        (struct zmk_widget_display_settings *)lv_mem_alloc(sizeof(struct zmk_widget_display_settings));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for display_settings_widget");
        return NULL;
    }

    memset(widget, 0, sizeof(struct zmk_widget_display_settings));

    int ret = zmk_widget_display_settings_init(widget, parent);
    if (ret != 0) {
        lv_mem_free(widget);
        return NULL;
    }

    LOG_DBG("Display settings widget created successfully");
    return widget;
}

void zmk_widget_display_settings_destroy(struct zmk_widget_display_settings *widget) {
    LOG_DBG("Destroying display settings widget");

    if (!widget) return;

    if (widget->obj) {
        lv_obj_del(widget->obj);
        widget->obj = NULL;
    }

    lv_mem_free(widget);
}

// ========== Widget Control Functions ==========

void zmk_widget_display_settings_show(struct zmk_widget_display_settings *widget) {
    LOG_INF("⚙️ Showing display settings");

    if (!widget || !widget->obj) return;

    // Restore slider state based on current auto brightness setting
    // CRITICAL: Widget is reused, must sync UI state when showing
    if (widget->auto_brightness_enabled) {
        // Auto brightness ON: disable manual slider
        lv_obj_add_state(widget->brightness_slider, LV_STATE_DISABLED);
        lv_obj_set_style_opa(widget->brightness_slider, LV_OPA_50, 0);
    } else {
        // Auto brightness OFF: enable manual slider
        lv_obj_clear_state(widget->brightness_slider, LV_STATE_DISABLED);
        lv_obj_set_style_opa(widget->brightness_slider, LV_OPA_COVER, 0);
    }

    lv_obj_move_foreground(widget->obj);
    lv_obj_clear_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
}

void zmk_widget_display_settings_hide(struct zmk_widget_display_settings *widget) {
    LOG_INF("⚙️ Hiding display settings");

    if (!widget || !widget->obj) return;

    lv_obj_add_flag(widget->obj, LV_OBJ_FLAG_HIDDEN);
}

// ========== Getter Functions ==========

uint8_t zmk_widget_display_settings_get_brightness(struct zmk_widget_display_settings *widget) {
    return widget ? widget->manual_brightness : 65;
}

bool zmk_widget_display_settings_get_auto_brightness(struct zmk_widget_display_settings *widget) {
    return widget ? widget->auto_brightness_enabled : false;
}

bool zmk_widget_display_settings_get_battery_visible(struct zmk_widget_display_settings *widget) {
    return widget ? widget->battery_widget_visible : true;
}

uint8_t zmk_widget_display_settings_get_max_layers(struct zmk_widget_display_settings *widget) {
    return widget ? widget->max_layers : 7;
}
