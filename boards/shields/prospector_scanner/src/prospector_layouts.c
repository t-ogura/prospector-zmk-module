/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Prospector Display Layouts - Scanner Mode Implementation
 *
 * Inspired by carrefinho's feat/new-status-screens branch:
 * https://github.com/carrefinho/prospector-zmk-module/tree/feat/new-status-screens
 *
 * Original layout designs by carrefinho, adapted for scanner mode
 * to visualize Periodic Advertising data from connected keyboards.
 *
 * License: MIT (same as original prospector-zmk-module)
 */

#include "prospector_layouts.h"
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(prospector_layouts, CONFIG_ZMK_LOG_LEVEL);

/* Display dimensions (280x240 landscape) */
#define DISPLAY_WIDTH   280
#define DISPLAY_HEIGHT  240

/* Colors - matching carrefinho's design palette */
#define COLOR_BG            lv_color_hex(0x000000)
#define COLOR_TEXT_PRIMARY  lv_color_hex(0xFFFFFF)
#define COLOR_TEXT_SECONDARY lv_color_hex(0x909090)
#define COLOR_ACCENT        lv_color_hex(0x4CAF50)
#define COLOR_BATTERY_FULL  lv_color_hex(0x4CAF50)
#define COLOR_BATTERY_MED   lv_color_hex(0xFFC107)
#define COLOR_BATTERY_LOW   lv_color_hex(0xF44336)
#define COLOR_MOD_ACTIVE    lv_color_hex(0x2196F3)
#define COLOR_MOD_INACTIVE  lv_color_hex(0x333333)
#define COLOR_DOT_ACTIVE    lv_color_hex(0xFFFFFF)
#define COLOR_DOT_INACTIVE  lv_color_hex(0x404040)

/* Layout state */
static lv_obj_t *parent_obj = NULL;
static lv_obj_t *layout_container = NULL;
static prospector_layout_t current_layout = PROSPECTOR_LAYOUT_CLASSIC;
static struct prospector_keyboard_data cached_data = {0};

/* Widget references for each layout */
static struct {
    /* Classic layout */
    lv_obj_t *classic_layer_label;
    lv_obj_t *classic_kb_name;
    lv_obj_t *classic_battery_bar;

    /* Field layout */
    lv_obj_t *field_layer_label;
    lv_obj_t *field_kb_name;
    lv_obj_t *field_wpm_label;
    lv_obj_t *field_battery_left;
    lv_obj_t *field_battery_right;
    lv_obj_t *field_mod_container;
    lv_obj_t *field_mods[4];  /* Ctrl, Shift, Alt, GUI */

    /* Operator layout */
    lv_obj_t *operator_dots[10];
    lv_obj_t *operator_wpm_label;
    lv_obj_t *operator_battery_icon;
    lv_obj_t *operator_profile_label;

    /* Radii layout */
    lv_obj_t *radii_wheel;
    lv_obj_t *radii_layer_label;
    lv_obj_t *radii_battery_arc;

    /* Common */
    lv_obj_t *layout_name_label;
} widgets;

/* Layout names */
static const char *layout_names[] = {
    "Classic",
    "Field",
    "Operator",
    "Radii"
};

/* Forward declarations */
static void create_classic_layout(void);
static void create_field_layout(void);
static void create_operator_layout(void);
static void create_radii_layout(void);
static void update_classic_layout(const struct prospector_keyboard_data *data);
static void update_field_layout(const struct prospector_keyboard_data *data);
static void update_operator_layout(const struct prospector_keyboard_data *data);
static void update_radii_layout(const struct prospector_keyboard_data *data);
static void destroy_current_layout(void);

/* ========== Helper Functions ========== */

static lv_color_t get_battery_color(uint8_t level) {
    if (level > 50) return COLOR_BATTERY_FULL;
    if (level > 20) return COLOR_BATTERY_MED;
    return COLOR_BATTERY_LOW;
}

static const char *get_layer_display_name(const struct prospector_keyboard_data *data) {
    /* Use current layer name from dynamic packet if available */
    if (data->current_layer_name[0] != '\0') {
        return data->current_layer_name;
    }

    /* Fallback to cached static data */
    if (data->active_layer < 10 && data->layer_names[data->active_layer][0] != '\0') {
        return data->layer_names[data->active_layer];
    }

    /* Ultimate fallback: layer number */
    static char fallback[16];
    snprintf(fallback, sizeof(fallback), "Layer %d", data->active_layer);
    return fallback;
}

/* ========== Classic Layout ========== */
/* Large centered layer name - inspired by carrefinho's FR_Regular_48 roller style */

static void create_classic_layout(void) {
    /* Keyboard name at top */
    widgets.classic_kb_name = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.classic_kb_name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(widgets.classic_kb_name, COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(widgets.classic_kb_name, "Waiting...");
    lv_obj_align(widgets.classic_kb_name, LV_ALIGN_TOP_MID, 0, 20);

    /* Large centered layer name */
    widgets.classic_layer_label = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.classic_layer_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(widgets.classic_layer_label, COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(widgets.classic_layer_label, "---");
    lv_obj_align(widgets.classic_layer_label, LV_ALIGN_CENTER, 0, -10);

    /* Battery bar at bottom */
    widgets.classic_battery_bar = lv_bar_create(layout_container);
    lv_obj_set_size(widgets.classic_battery_bar, 200, 12);
    lv_bar_set_range(widgets.classic_battery_bar, 0, 100);
    lv_bar_set_value(widgets.classic_battery_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(widgets.classic_battery_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(widgets.classic_battery_bar, COLOR_BATTERY_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_radius(widgets.classic_battery_bar, 6, 0);
    lv_obj_align(widgets.classic_battery_bar, LV_ALIGN_BOTTOM_MID, 0, -40);

    LOG_DBG("Classic layout created");
}

static void update_classic_layout(const struct prospector_keyboard_data *data) {
    if (!widgets.classic_layer_label) return;

    /* Update keyboard name */
    if (data->has_static_data && data->keyboard_name[0] != '\0') {
        lv_label_set_text(widgets.classic_kb_name, data->keyboard_name);
    }

    /* Update layer name with animation-like effect */
    const char *layer_name = get_layer_display_name(data);
    lv_label_set_text(widgets.classic_layer_label, layer_name);
    lv_obj_align(widgets.classic_layer_label, LV_ALIGN_CENTER, 0, -10);

    /* Update battery bar */
    lv_bar_set_value(widgets.classic_battery_bar, data->battery_level, LV_ANIM_ON);
    lv_obj_set_style_bg_color(widgets.classic_battery_bar,
                              get_battery_color(data->battery_level), LV_PART_INDICATOR);
}

/* ========== Field Layout ========== */
/* Clean modern layout with all status info */

static void create_field_layout(void) {
    /* Top row: keyboard name and connection status */
    widgets.field_kb_name = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.field_kb_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widgets.field_kb_name, COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(widgets.field_kb_name, "Waiting...");
    lv_obj_align(widgets.field_kb_name, LV_ALIGN_TOP_LEFT, 10, 10);

    /* WPM display (top right) */
    widgets.field_wpm_label = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.field_wpm_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widgets.field_wpm_label, COLOR_ACCENT, 0);
    lv_label_set_text(widgets.field_wpm_label, "0 WPM");
    lv_obj_align(widgets.field_wpm_label, LV_ALIGN_TOP_RIGHT, -10, 10);

    /* Layer name (center, large) */
    widgets.field_layer_label = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.field_layer_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(widgets.field_layer_label, COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(widgets.field_layer_label, "---");
    lv_obj_align(widgets.field_layer_label, LV_ALIGN_CENTER, 0, -30);

    /* Modifier keys row */
    widgets.field_mod_container = lv_obj_create(layout_container);
    lv_obj_remove_style_all(widgets.field_mod_container);
    lv_obj_set_size(widgets.field_mod_container, 200, 30);
    lv_obj_align(widgets.field_mod_container, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_flex_flow(widgets.field_mod_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(widgets.field_mod_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(widgets.field_mod_container, 10, 0);

    const char *mod_labels[] = {"Ctrl", "Shift", "Alt", "GUI"};
    for (int i = 0; i < 4; i++) {
        widgets.field_mods[i] = lv_label_create(widgets.field_mod_container);
        lv_obj_set_style_text_font(widgets.field_mods[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(widgets.field_mods[i], COLOR_MOD_INACTIVE, 0);
        lv_label_set_text(widgets.field_mods[i], mod_labels[i]);
    }

    /* Battery indicators (bottom) */
    widgets.field_battery_left = lv_bar_create(layout_container);
    lv_obj_set_size(widgets.field_battery_left, 100, 10);
    lv_bar_set_range(widgets.field_battery_left, 0, 100);
    lv_obj_set_style_bg_color(widgets.field_battery_left, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(widgets.field_battery_left, COLOR_BATTERY_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_radius(widgets.field_battery_left, 5, 0);
    lv_obj_align(widgets.field_battery_left, LV_ALIGN_BOTTOM_LEFT, 20, -30);

    widgets.field_battery_right = lv_bar_create(layout_container);
    lv_obj_set_size(widgets.field_battery_right, 100, 10);
    lv_bar_set_range(widgets.field_battery_right, 0, 100);
    lv_obj_set_style_bg_color(widgets.field_battery_right, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(widgets.field_battery_right, COLOR_BATTERY_FULL, LV_PART_INDICATOR);
    lv_obj_set_style_radius(widgets.field_battery_right, 5, 0);
    lv_obj_align(widgets.field_battery_right, LV_ALIGN_BOTTOM_RIGHT, -20, -30);

    LOG_DBG("Field layout created");
}

static void update_field_layout(const struct prospector_keyboard_data *data) {
    if (!widgets.field_layer_label) return;

    /* Update keyboard name */
    if (data->has_static_data && data->keyboard_name[0] != '\0') {
        lv_label_set_text(widgets.field_kb_name, data->keyboard_name);
    }

    /* Update WPM */
    char wpm_str[16];
    snprintf(wpm_str, sizeof(wpm_str), "%d WPM", data->wpm_value);
    lv_label_set_text(widgets.field_wpm_label, wpm_str);

    /* Update layer name */
    lv_label_set_text(widgets.field_layer_label, get_layer_display_name(data));
    lv_obj_align(widgets.field_layer_label, LV_ALIGN_CENTER, 0, -30);

    /* Update modifiers */
    uint8_t mods = data->modifier_flags;
    lv_obj_set_style_text_color(widgets.field_mods[0],
        (mods & 0x11) ? COLOR_MOD_ACTIVE : COLOR_MOD_INACTIVE, 0);  /* Ctrl */
    lv_obj_set_style_text_color(widgets.field_mods[1],
        (mods & 0x22) ? COLOR_MOD_ACTIVE : COLOR_MOD_INACTIVE, 0);  /* Shift */
    lv_obj_set_style_text_color(widgets.field_mods[2],
        (mods & 0x44) ? COLOR_MOD_ACTIVE : COLOR_MOD_INACTIVE, 0);  /* Alt */
    lv_obj_set_style_text_color(widgets.field_mods[3],
        (mods & 0x88) ? COLOR_MOD_ACTIVE : COLOR_MOD_INACTIVE, 0);  /* GUI */

    /* Update battery bars */
    lv_bar_set_value(widgets.field_battery_left, data->battery_level, LV_ANIM_ON);
    lv_obj_set_style_bg_color(widgets.field_battery_left,
        get_battery_color(data->battery_level), LV_PART_INDICATOR);

    uint8_t periph_bat = data->peripheral_battery[0];
    lv_bar_set_value(widgets.field_battery_right, periph_bat, LV_ANIM_ON);
    lv_obj_set_style_bg_color(widgets.field_battery_right,
        get_battery_color(periph_bat), LV_PART_INDICATOR);
}

/* ========== Operator Layout ========== */
/* Minimalist with dot indicators */

static void create_operator_layout(void) {
    /* Dot indicators for layers (center) */
    lv_obj_t *dot_container = lv_obj_create(layout_container);
    lv_obj_remove_style_all(dot_container);
    lv_obj_set_size(dot_container, 260, 20);
    lv_obj_align(dot_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(dot_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_container, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(dot_container, 8, 0);

    for (int i = 0; i < 10; i++) {
        widgets.operator_dots[i] = lv_obj_create(dot_container);
        lv_obj_remove_style_all(widgets.operator_dots[i]);
        lv_obj_set_size(widgets.operator_dots[i], 12, 12);
        lv_obj_set_style_radius(widgets.operator_dots[i], 6, 0);
        lv_obj_set_style_bg_color(widgets.operator_dots[i], COLOR_DOT_INACTIVE, 0);
        lv_obj_set_style_bg_opa(widgets.operator_dots[i], LV_OPA_COVER, 0);
    }

    /* WPM (top left) */
    widgets.operator_wpm_label = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.operator_wpm_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(widgets.operator_wpm_label, COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(widgets.operator_wpm_label, "0");
    lv_obj_align(widgets.operator_wpm_label, LV_ALIGN_TOP_LEFT, 20, 20);

    /* Profile indicator (top right) */
    widgets.operator_profile_label = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.operator_profile_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(widgets.operator_profile_label, COLOR_ACCENT, 0);
    lv_label_set_text(widgets.operator_profile_label, "BLE 0");
    lv_obj_align(widgets.operator_profile_label, LV_ALIGN_TOP_RIGHT, -20, 20);

    /* Battery percentage (bottom center) */
    widgets.operator_battery_icon = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.operator_battery_icon, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(widgets.operator_battery_icon, COLOR_BATTERY_FULL, 0);
    lv_label_set_text(widgets.operator_battery_icon, "-- %");
    lv_obj_align(widgets.operator_battery_icon, LV_ALIGN_BOTTOM_MID, 0, -30);

    LOG_DBG("Operator layout created");
}

static void update_operator_layout(const struct prospector_keyboard_data *data) {
    if (!widgets.operator_dots[0]) return;

    /* Update dot indicators */
    uint8_t layer_count = (data->layer_count > 0 && data->layer_count <= 10)
                          ? data->layer_count : 7;
    for (int i = 0; i < 10; i++) {
        if (i < layer_count) {
            lv_obj_clear_flag(widgets.operator_dots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(widgets.operator_dots[i],
                (i == data->active_layer) ? COLOR_DOT_ACTIVE : COLOR_DOT_INACTIVE, 0);
        } else {
            lv_obj_add_flag(widgets.operator_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Update WPM */
    char wpm_str[8];
    snprintf(wpm_str, sizeof(wpm_str), "%d", data->wpm_value);
    lv_label_set_text(widgets.operator_wpm_label, wpm_str);

    /* Update profile */
    char profile_str[16];
    if (data->usb_connected) {
        snprintf(profile_str, sizeof(profile_str), "USB");
    } else {
        snprintf(profile_str, sizeof(profile_str), "BLE %d", data->profile_slot);
    }
    lv_label_set_text(widgets.operator_profile_label, profile_str);

    /* Update battery */
    char bat_str[8];
    snprintf(bat_str, sizeof(bat_str), "%d%%", data->battery_level);
    lv_label_set_text(widgets.operator_battery_icon, bat_str);
    lv_obj_set_style_text_color(widgets.operator_battery_icon,
        get_battery_color(data->battery_level), 0);
}

/* ========== Radii Layout ========== */
/* Simplified circular-inspired layout (arc widget not available in config) */

static void create_radii_layout(void) {
    /* Large centered layer name */
    widgets.radii_layer_label = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.radii_layer_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(widgets.radii_layer_label, COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(widgets.radii_layer_label, "---");
    lv_obj_align(widgets.radii_layer_label, LV_ALIGN_CENTER, 0, -20);

    /* Layer number indicator */
    widgets.radii_wheel = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.radii_wheel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(widgets.radii_wheel, COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(widgets.radii_wheel, "Layer 0");
    lv_obj_align(widgets.radii_wheel, LV_ALIGN_CENTER, 0, 30);

    /* Battery percentage at bottom */
    widgets.radii_battery_arc = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.radii_battery_arc, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(widgets.radii_battery_arc, COLOR_BATTERY_FULL, 0);
    lv_label_set_text(widgets.radii_battery_arc, "-- %");
    lv_obj_align(widgets.radii_battery_arc, LV_ALIGN_BOTTOM_MID, 0, -30);

    LOG_DBG("Radii layout created (simplified - no arc widget)");
}

static void update_radii_layout(const struct prospector_keyboard_data *data) {
    if (!widgets.radii_layer_label) return;

    /* Update layer name */
    lv_label_set_text(widgets.radii_layer_label, get_layer_display_name(data));
    lv_obj_align(widgets.radii_layer_label, LV_ALIGN_CENTER, 0, -20);

    /* Update layer number */
    char layer_str[16];
    snprintf(layer_str, sizeof(layer_str), "Layer %d", data->active_layer);
    lv_label_set_text(widgets.radii_wheel, layer_str);

    /* Update battery label */
    char bat_str[8];
    snprintf(bat_str, sizeof(bat_str), "%d%%", data->battery_level);
    lv_label_set_text(widgets.radii_battery_arc, bat_str);
    lv_obj_set_style_text_color(widgets.radii_battery_arc,
        get_battery_color(data->battery_level), 0);
}

/* ========== Layout Management ========== */

static void destroy_current_layout(void) {
    /* Clear all widget pointers */
    memset(&widgets, 0, sizeof(widgets));

    /* Clean the container */
    if (layout_container) {
        lv_obj_clean(layout_container);
    }
}

static void create_layout(prospector_layout_t layout) {
    destroy_current_layout();

    if (!layout_container) {
        LOG_ERR("Layout container is NULL");
        return;
    }

    switch (layout) {
    case PROSPECTOR_LAYOUT_CLASSIC:
        create_classic_layout();
        break;
    case PROSPECTOR_LAYOUT_FIELD:
        create_field_layout();
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        create_operator_layout();
        break;
    case PROSPECTOR_LAYOUT_RADII:
        create_radii_layout();
        break;
    default:
        LOG_WRN("Unknown layout: %d", layout);
        create_classic_layout();
        break;
    }

    /* Add layout name indicator at bottom */
    widgets.layout_name_label = lv_label_create(layout_container);
    lv_obj_set_style_text_font(widgets.layout_name_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(widgets.layout_name_label, COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(widgets.layout_name_label, layout_names[layout]);
    lv_obj_align(widgets.layout_name_label, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* Apply cached data if available */
    if (cached_data.has_dynamic_data) {
        prospector_layouts_update(&cached_data);
    }

    LOG_INF("Created layout: %s", layout_names[layout]);
}

/* ========== Public API ========== */

void prospector_layouts_init(lv_obj_t *parent) {
    if (!parent) {
        LOG_ERR("Parent object is NULL");
        return;
    }

    parent_obj = parent;

    /* Create main container */
    layout_container = lv_obj_create(parent);
    lv_obj_remove_style_all(layout_container);
    lv_obj_set_size(layout_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(layout_container, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(layout_container, LV_OPA_COVER, 0);
    lv_obj_align(layout_container, LV_ALIGN_CENTER, 0, 0);

    /* Create initial layout */
    create_layout(current_layout);

    LOG_INF("Prospector layouts initialized");
}

void prospector_layouts_destroy(void) {
    destroy_current_layout();

    if (layout_container) {
        lv_obj_del(layout_container);
        layout_container = NULL;
    }

    parent_obj = NULL;
    memset(&cached_data, 0, sizeof(cached_data));

    LOG_INF("Prospector layouts destroyed");
}

void prospector_layouts_set_style(prospector_layout_t layout) {
    if (layout >= PROSPECTOR_LAYOUT_COUNT) {
        LOG_WRN("Invalid layout: %d", layout);
        return;
    }

    if (layout == current_layout) {
        return;
    }

    current_layout = layout;
    create_layout(layout);
}

prospector_layout_t prospector_layouts_get_style(void) {
    return current_layout;
}

void prospector_layouts_next(void) {
    prospector_layout_t next = (current_layout + 1) % PROSPECTOR_LAYOUT_COUNT;
    prospector_layouts_set_style(next);
}

void prospector_layouts_prev(void) {
    prospector_layout_t prev = (current_layout + PROSPECTOR_LAYOUT_COUNT - 1) % PROSPECTOR_LAYOUT_COUNT;
    prospector_layouts_set_style(prev);
}

void prospector_layouts_update(const struct prospector_keyboard_data *data) {
    if (!data || !layout_container) {
        return;
    }

    /* Cache the data */
    memcpy(&cached_data, data, sizeof(cached_data));

    /* Update current layout */
    switch (current_layout) {
    case PROSPECTOR_LAYOUT_CLASSIC:
        update_classic_layout(data);
        break;
    case PROSPECTOR_LAYOUT_FIELD:
        update_field_layout(data);
        break;
    case PROSPECTOR_LAYOUT_OPERATOR:
        update_operator_layout(data);
        break;
    case PROSPECTOR_LAYOUT_RADII:
        update_radii_layout(data);
        break;
    default:
        break;
    }
}

const char *prospector_layouts_get_name(prospector_layout_t layout) {
    if (layout >= PROSPECTOR_LAYOUT_COUNT) {
        return "Unknown";
    }
    return layout_names[layout];
}
