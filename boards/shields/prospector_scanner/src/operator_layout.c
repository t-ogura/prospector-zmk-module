/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Operator Layout for Scanner Mode
 * Direct port from carrefinho's feat/new-status-screens
 */

#include "operator_layout.h"
#include "fonts_carrefinho.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(operator_layout, CONFIG_ZMK_LOG_LEVEL);

/* External function to get current max layers from display settings */
extern uint8_t display_settings_get_max_layers(void);

/* ========== Color Palette Structure ========== */
typedef struct {
    /* Modifier colors */
    uint32_t mod_active;
    uint32_t mod_inactive;
    uint32_t mod_separator;
    /* WPM colors */
    uint32_t wpm_bar_active;
    uint32_t wpm_bar_inactive;
    uint32_t wpm_text;
    /* Layer colors */
    uint32_t layer_text;
    uint32_t layer_dot_active;
    uint32_t layer_dot_inactive;
    /* Battery colors */
    uint32_t battery_fill;
    uint32_t battery_ring;
    uint32_t battery_label;
    uint32_t battery_low_fill;
    uint32_t battery_low_ring;
    /* Output indicator colors */
    uint32_t usb_active_bg;
    uint32_t usb_inactive_bg;
    uint32_t ble_active_bg;
    uint32_t ble_inactive_bg;
    uint32_t output_active_text;
    uint32_t output_inactive_text;
    uint32_t slot_active_bg;
    uint32_t slot_inactive_bg;
    /* Theme name */
    const char *name;
} operator_color_palette_t;

/* ========== 4 Color Palettes ========== */
static const operator_color_palette_t color_palettes[4] = {
    /* Teal Theme (default - carrefinho original) */
    {
        .mod_active = 0xb1e5f0,      /* Light cyan */
        .mod_inactive = 0x3b527c,    /* Dark blue */
        .mod_separator = 0x606060,
        .wpm_bar_active = 0xc2526a,  /* Pink/magenta */
        .wpm_bar_inactive = 0x242424,
        .wpm_text = 0xc2526a,
        .layer_text = 0xffffff,
        .layer_dot_active = 0xe0e0e0,
        .layer_dot_inactive = 0x575757,
        .battery_fill = 0x54806c,    /* Teal green */
        .battery_ring = 0x2a4036,
        .battery_label = 0xffffff,
        .battery_low_fill = 0xC08040,
        .battery_low_ring = 0x584028,
        .usb_active_bg = 0xb9b9a7,   /* Beige */
        .usb_inactive_bg = 0x4F4F40,
        .ble_active_bg = 0x569FA7,   /* Teal */
        .ble_inactive_bg = 0x353f40,
        .output_active_text = 0x000000,
        .output_inactive_text = 0x7b7d93,
        .slot_active_bg = 0x7b7d93,
        .slot_inactive_bg = 0x353640,
        .name = "Teal"
    },
    /* Warm Theme - Orange/Amber tones */
    {
        .mod_active = 0xFFD699,      /* Light orange */
        .mod_inactive = 0x7A5230,    /* Dark brown */
        .mod_separator = 0x606060,
        .wpm_bar_active = 0xFF8C42,  /* Orange */
        .wpm_bar_inactive = 0x2A2018,
        .wpm_text = 0xFF8C42,
        .layer_text = 0xFFE4C4,      /* Bisque */
        .layer_dot_active = 0xFFD699,
        .layer_dot_inactive = 0x5A4A3A,
        .battery_fill = 0xD4A056,    /* Gold */
        .battery_ring = 0x6A5028,
        .battery_label = 0xffffff,
        .battery_low_fill = 0xFF6B35,
        .battery_low_ring = 0x7A3018,
        .usb_active_bg = 0xE8C87A,   /* Gold */
        .usb_inactive_bg = 0x5A4A30,
        .ble_active_bg = 0xE07040,   /* Coral */
        .ble_inactive_bg = 0x4A3028,
        .output_active_text = 0x000000,
        .output_inactive_text = 0x8A7A6A,
        .slot_active_bg = 0xB08050,
        .slot_inactive_bg = 0x3A3028,
        .name = "Warm"
    },
    /* Purple Theme - Violet/Lavender tones */
    {
        .mod_active = 0xD4AAFF,      /* Light purple */
        .mod_inactive = 0x4A3070,    /* Dark purple */
        .mod_separator = 0x606060,
        .wpm_bar_active = 0xAA66CC,  /* Purple */
        .wpm_bar_inactive = 0x1E1828,
        .wpm_text = 0xAA66CC,
        .layer_text = 0xE8D8FF,      /* Lavender */
        .layer_dot_active = 0xD4AAFF,
        .layer_dot_inactive = 0x4A4060,
        .battery_fill = 0x8866AA,    /* Medium purple */
        .battery_ring = 0x443355,
        .battery_label = 0xffffff,
        .battery_low_fill = 0xCC5599,
        .battery_low_ring = 0x662244,
        .usb_active_bg = 0xC0A0D0,   /* Light violet */
        .usb_inactive_bg = 0x4A3A50,
        .ble_active_bg = 0x7766BB,   /* Blue-purple */
        .ble_inactive_bg = 0x3A3050,
        .output_active_text = 0x000000,
        .output_inactive_text = 0x9080A0,
        .slot_active_bg = 0x8A7AAA,
        .slot_inactive_bg = 0x353045,
        .name = "Purple"
    },
    /* Mono Theme - Grayscale with cyan accent */
    {
        .mod_active = 0x00FFFF,      /* Cyan accent */
        .mod_inactive = 0x606060,    /* Gray */
        .mod_separator = 0x404040,
        .wpm_bar_active = 0x00CCCC,  /* Dark cyan */
        .wpm_bar_inactive = 0x1A1A1A,
        .wpm_text = 0x00CCCC,
        .layer_text = 0xE0E0E0,      /* Light gray */
        .layer_dot_active = 0xFFFFFF,
        .layer_dot_inactive = 0x404040,
        .battery_fill = 0x808080,    /* Gray */
        .battery_ring = 0x404040,
        .battery_label = 0xffffff,
        .battery_low_fill = 0xCC4444,
        .battery_low_ring = 0x662222,
        .usb_active_bg = 0xA0A0A0,   /* Light gray */
        .usb_inactive_bg = 0x404040,
        .ble_active_bg = 0x00AAAA,   /* Teal accent */
        .ble_inactive_bg = 0x303030,
        .output_active_text = 0x000000,
        .output_inactive_text = 0x808080,
        .slot_active_bg = 0x707070,
        .slot_inactive_bg = 0x303030,
        .name = "Mono"
    }
};

#define PALETTE_COUNT 4
static uint8_t current_palette = 0;

/* Original positions from carrefinho - no offset needed (hardware handles y-offset) */
/* Display: 240x280 with hardware y-offset=20, software sees 240x240 */

/* ========== Static Widget Storage ========== */

static lv_obj_t *layout_container = NULL;

/* Modifier indicator widgets */
static struct {
    lv_obj_t *container;
    lv_obj_t *labels[4];  /* CTRL, ALT, SHIFT, GUI */
} modifier_widgets;

/* WPM meter widgets */
static struct {
    lv_obj_t *container;
    lv_obj_t *bars[WPM_BAR_COUNT];
    lv_obj_t *peak_indicator;
    lv_obj_t *wpm_label;
    lv_obj_t *layer_label;
} wpm_widgets;

/* Layer display widgets */
static struct {
    lv_obj_t *container;
    lv_obj_t *dots[LAYER_DOT_MAX];
    int dot_count;
} layer_widgets;

/* Battery circles widgets */
static struct {
    lv_obj_t *container;
    lv_obj_t *central_arc;
    lv_obj_t *central_label_box;
    lv_obj_t *central_label;
    lv_obj_t *central_battery_label;
    lv_obj_t *peripheral_arc;
    lv_obj_t *peripheral_label_box;
    lv_obj_t *peripheral_label;
    lv_obj_t *peripheral_battery_label;
} battery_widgets;

/* Output indicator widgets */
static struct {
    lv_obj_t *container;
    lv_obj_t *usb_box;
    lv_obj_t *usb_label;
    lv_obj_t *ble_box;
    lv_obj_t *ble_label;
    lv_obj_t *slot_boxes[5];
    lv_obj_t *slot_labels[5];
} output_widgets;

/* BLE slot animation state */
static struct {
    lv_timer_t *anim_timer;     /* Timer for blink/fade animation */
    uint8_t active_slot;
    ble_profile_state_t current_state;
    uint8_t fade_value;         /* 0-255 for fade animation */
    bool fade_direction;        /* true = fading in, false = fading out */
    bool blink_phase;           /* true = on, false = off for simple blink */
} ble_anim_state = {
    .anim_timer = NULL,
    .active_slot = 0,
    .current_state = BLE_PROFILE_STATE_CONNECTED,
    .fade_value = 255,
    .fade_direction = false,
    .blink_phase = true
};

/* Cached state for change detection (must be before create functions) */
static struct {
    uint8_t active_layer;
    char layer_name[8];
    uint8_t battery_level;
    bool battery_connected;
    uint8_t peripheral_battery;
    bool peripheral_connected;
    uint8_t wpm;
    uint8_t modifier_flags;
    bool usb_connected;
    uint8_t ble_profile;
    bool ble_connected;
    bool ble_bonded;
    bool initialized;
} cached_state = {0};

/* ========== Modifier Indicator ========== */

static const char *modifier_texts[] = {"CTRL", "ALT", "SHFT", "GUI"};

/* Separator objects stored for palette updates */
static lv_obj_t *mod_separators[3] = {NULL};

static void create_modifier_indicator(lv_obj_t *parent) {
    const operator_color_palette_t *p = &color_palettes[current_palette];

    modifier_widgets.container = lv_obj_create(parent);
    lv_obj_set_size(modifier_widgets.container, 230, 24);  /* Original: 230x24 */
    lv_obj_set_pos(modifier_widgets.container, 25, 8);     /* Original: x=25, y=8 */
    lv_obj_set_style_bg_opa(modifier_widgets.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(modifier_widgets.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(modifier_widgets.container, 0, LV_PART_MAIN);

    lv_obj_set_flex_flow(modifier_widgets.container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(modifier_widgets.container, LV_FLEX_ALIGN_SPACE_BETWEEN,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 4; i++) {
        modifier_widgets.labels[i] = lv_label_create(modifier_widgets.container);
        lv_label_set_text(modifier_widgets.labels[i], modifier_texts[i]);
        lv_obj_set_style_text_font(modifier_widgets.labels[i], &FG_Medium_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(modifier_widgets.labels[i],
                                    lv_color_hex(p->mod_inactive), LV_PART_MAIN);

        /* Add separator after each except last */
        if (i < 3) {
            lv_obj_t *sep = lv_obj_create(modifier_widgets.container);
            lv_obj_set_size(sep, 2, 24);
            lv_obj_set_style_bg_color(sep, lv_color_hex(p->mod_separator), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(sep, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(sep, 0, LV_PART_MAIN);
            mod_separators[i] = sep;
        }
    }
}

static void update_modifier_indicator(uint8_t modifier_flags) {
    const operator_color_palette_t *p = &color_palettes[current_palette];

    bool mods[4] = {
        (modifier_flags & 0x11) != 0,  /* CTRL (bit 0 or 4) */
        (modifier_flags & 0x44) != 0,  /* ALT (bit 2 or 6) */
        (modifier_flags & 0x22) != 0,  /* SHIFT (bit 1 or 5) */
        (modifier_flags & 0x88) != 0,  /* GUI (bit 3 or 7) */
    };

    for (int i = 0; i < 4; i++) {
        lv_color_t color = mods[i] ? lv_color_hex(p->mod_active)
                                   : lv_color_hex(p->mod_inactive);
        lv_obj_set_style_text_color(modifier_widgets.labels[i], color, LV_PART_MAIN);
    }
}

/* ========== WPM Meter with Layer Name ========== */

static void create_wpm_meter(lv_obj_t *parent) {
    wpm_widgets.container = lv_obj_create(parent);
    lv_obj_set_size(wpm_widgets.container, 260, 90);  /* Original: 260x90 */
    lv_obj_set_pos(wpm_widgets.container, 10, 42);    /* Original: x=10, y=42 */
    lv_obj_set_style_bg_opa(wpm_widgets.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(wpm_widgets.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wpm_widgets.container, 0, LV_PART_MAIN);

    int bar_width = 8;
    int bar_gap = 2;
    int bar_height = 90;
    int total_width = WPM_BAR_COUNT * bar_width + (WPM_BAR_COUNT - 1) * bar_gap;
    int start_x = (260 - total_width) / 2;  /* Use original width 260 */

    /* Create WPM bars */
    for (int i = 0; i < WPM_BAR_COUNT; i++) {
        wpm_widgets.bars[i] = lv_obj_create(wpm_widgets.container);
        lv_obj_set_size(wpm_widgets.bars[i], bar_width, bar_height);
        lv_obj_set_pos(wpm_widgets.bars[i], start_x + i * (bar_width + bar_gap), 0);
        lv_obj_set_style_bg_color(wpm_widgets.bars[i],
                                  lv_color_hex(DISPLAY_COLOR_WPM_BAR_INACTIVE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(wpm_widgets.bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(wpm_widgets.bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(wpm_widgets.bars[i], 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(wpm_widgets.bars[i], 0, LV_PART_MAIN);
    }

    /* Peak indicator */
    wpm_widgets.peak_indicator = lv_obj_create(wpm_widgets.container);
    lv_obj_set_size(wpm_widgets.peak_indicator, 4, bar_height);
    lv_obj_set_style_bg_color(wpm_widgets.peak_indicator, lv_color_hex(0x505050), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wpm_widgets.peak_indicator, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wpm_widgets.peak_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(wpm_widgets.peak_indicator, 1, LV_PART_MAIN);
    lv_obj_add_flag(wpm_widgets.peak_indicator, LV_OBJ_FLAG_HIDDEN);

    /* WPM label */
    wpm_widgets.wpm_label = lv_label_create(wpm_widgets.container);
    lv_label_set_text(wpm_widgets.wpm_label, "0");
    lv_obj_set_style_text_font(wpm_widgets.wpm_label, &FR_Medium_32, LV_PART_MAIN);
    lv_obj_set_style_text_color(wpm_widgets.wpm_label,
                                lv_color_hex(DISPLAY_COLOR_WPM_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(wpm_widgets.wpm_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wpm_widgets.wpm_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(wpm_widgets.wpm_label, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(wpm_widgets.wpm_label, 4, LV_PART_MAIN);
    lv_obj_align(wpm_widgets.wpm_label, LV_ALIGN_TOP_LEFT, -7, -9);

    /* Layer label (shown over WPM bars) */
    wpm_widgets.layer_label = lv_label_create(wpm_widgets.container);
    lv_label_set_text(wpm_widgets.layer_label, "");
    lv_obj_set_style_text_font(wpm_widgets.layer_label, &DINishExpanded_Light_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(wpm_widgets.layer_label,
                                lv_color_hex(DISPLAY_COLOR_LAYER_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(wpm_widgets.layer_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wpm_widgets.layer_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(wpm_widgets.layer_label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(wpm_widgets.layer_label, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(wpm_widgets.layer_label, 3, LV_PART_MAIN);
    lv_obj_align(wpm_widgets.layer_label, LV_ALIGN_BOTTOM_RIGHT, 9, 7);
}

static void update_wpm_meter(uint8_t wpm, const char *layer_name) {
    const operator_color_palette_t *p = &color_palettes[current_palette];

    /* Update WPM bars */
    int active_bars = (wpm * WPM_BAR_COUNT) / WPM_MAX;
    if (active_bars > WPM_BAR_COUNT) active_bars = WPM_BAR_COUNT;

    for (int i = 0; i < WPM_BAR_COUNT; i++) {
        lv_color_t color = (i < active_bars)
            ? lv_color_hex(p->wpm_bar_active)
            : lv_color_hex(p->wpm_bar_inactive);
        lv_obj_set_style_bg_color(wpm_widgets.bars[i], color, LV_PART_MAIN);
    }

    /* Update WPM label */
    char wpm_text[8];
    snprintf(wpm_text, sizeof(wpm_text), "%d", wpm);
    lv_label_set_text(wpm_widgets.wpm_label, wpm_text);

    /* Update layer label */
    if (layer_name && layer_name[0]) {
        lv_label_set_text(wpm_widgets.layer_label, layer_name);
    } else {
        lv_label_set_text(wpm_widgets.layer_label, "BASE");
    }
}

/* ========== Layer Display (Dots) ========== */

static void create_layer_display(lv_obj_t *parent) {
    layer_widgets.container = lv_obj_create(parent);
    lv_obj_set_size(layer_widgets.container, 260, 6);  /* Original: 260x6 */
    lv_obj_set_pos(layer_widgets.container, 10, 142);  /* Original: x=10, y=142 */
    lv_obj_set_style_bg_opa(layer_widgets.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(layer_widgets.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(layer_widgets.container, 0, LV_PART_MAIN);

    /* Pre-create all possible dots (will be shown/hidden dynamically) */
    layer_widgets.dot_count = 0;  /* Will be set in update */
    for (int i = 0; i < LAYER_DOT_MAX; i++) {
        layer_widgets.dots[i] = lv_obj_create(layer_widgets.container);
        lv_obj_set_style_bg_color(layer_widgets.dots[i],
                                  lv_color_hex(DISPLAY_COLOR_LAYER_DOT_INACTIVE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(layer_widgets.dots[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(layer_widgets.dots[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(layer_widgets.dots[i], 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(layer_widgets.dots[i], 0, LV_PART_MAIN);
        lv_obj_add_flag(layer_widgets.dots[i], LV_OBJ_FLAG_HIDDEN);  /* Hidden initially */
    }
}

static void update_layer_display(uint8_t active_layer) {
    /* Get current max layers from display settings */
    int layer_count = display_settings_get_max_layers();
    if (layer_count > LAYER_DOT_MAX) layer_count = LAYER_DOT_MAX;
    if (layer_count < 1) layer_count = 1;

    /* Reconfigure dots if layer count changed */
    if (layer_count != layer_widgets.dot_count) {
        int dot_gap = 3;
        int dot_width = (260 - (layer_count - 1) * dot_gap) / layer_count;

        /* Update size and position for all dots */
        for (int i = 0; i < LAYER_DOT_MAX; i++) {
            if (i < layer_count) {
                lv_obj_set_size(layer_widgets.dots[i], dot_width, 6);
                lv_obj_set_pos(layer_widgets.dots[i], i * (dot_width + dot_gap), 0);
                lv_obj_clear_flag(layer_widgets.dots[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(layer_widgets.dots[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        layer_widgets.dot_count = layer_count;
    }

    /* Update colors */
    const operator_color_palette_t *p = &color_palettes[current_palette];
    for (int i = 0; i < layer_widgets.dot_count; i++) {
        lv_color_t color = (i == active_layer)
            ? lv_color_hex(p->layer_dot_active)
            : lv_color_hex(p->layer_dot_inactive);
        lv_obj_set_style_bg_color(layer_widgets.dots[i], color, LV_PART_MAIN);
    }
}

/* ========== Battery Circles ========== */

static void create_battery_arc(lv_obj_t *parent, lv_obj_t **arc, lv_obj_t **label_box,
                               lv_obj_t **label, lv_obj_t **battery_label,
                               int x_pos, const char *title) {
    int arc_size = 58;
    int arc_left = 60;

    /* Arc */
    *arc = lv_arc_create(parent);
    lv_obj_set_size(*arc, arc_size, arc_size);
    lv_obj_set_pos(*arc, x_pos, 2);
    lv_arc_set_range(*arc, 0, 100);
    lv_arc_set_value(*arc, 0);
    lv_arc_set_bg_angles(*arc, 0, 360);
    lv_arc_set_rotation(*arc, 270);
    lv_obj_set_style_arc_width(*arc, ARC_WIDTH_DISCONNECTED, LV_PART_MAIN);
    lv_obj_set_style_arc_width(*arc, ARC_WIDTH_DISCONNECTED, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(*arc, lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_RING), LV_PART_MAIN);
    lv_obj_set_style_arc_color(*arc, lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL), LV_PART_INDICATOR);
    lv_obj_remove_style(*arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(*arc, LV_OBJ_FLAG_CLICKABLE);

    /* Label box inside arc - not used for 2-peripheral layout */
    (void)label_box;
    (void)label;
    (void)battery_label;
    (void)title;
    (void)arc_left;
}

static void create_battery_circles(lv_obj_t *parent) {
    battery_widgets.container = lv_obj_create(parent);
    lv_obj_set_size(battery_widgets.container, 132, 62);  /* Original: 132x62 */
    lv_obj_set_pos(battery_widgets.container, 11, 170);   /* Original: x=11, y=170 */
    lv_obj_set_style_bg_opa(battery_widgets.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_widgets.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(battery_widgets.container, 0, LV_PART_MAIN);

    /* Two arcs side by side (like PERIPHERAL_COUNT == 2 in carrefinho) */
    int arc_size = 58;
    int y_center = (62 - arc_size) / 2;
    int spacing = 66;

    /* Central (left arc) */
    battery_widgets.central_arc = lv_arc_create(battery_widgets.container);
    lv_obj_set_size(battery_widgets.central_arc, arc_size, arc_size);
    lv_obj_set_pos(battery_widgets.central_arc, 0, y_center);
    lv_arc_set_range(battery_widgets.central_arc, 0, 100);
    lv_arc_set_value(battery_widgets.central_arc, 0);
    lv_arc_set_bg_angles(battery_widgets.central_arc, 270, 180);
    lv_arc_set_rotation(battery_widgets.central_arc, 0);
    lv_obj_set_style_arc_width(battery_widgets.central_arc, ARC_WIDTH_DISCONNECTED, LV_PART_MAIN);
    lv_obj_set_style_arc_width(battery_widgets.central_arc, ARC_WIDTH_DISCONNECTED, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(battery_widgets.central_arc,
                               lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_RING), LV_PART_MAIN);
    lv_obj_set_style_arc_color(battery_widgets.central_arc,
                               lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL), LV_PART_INDICATOR);
    lv_obj_remove_style(battery_widgets.central_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(battery_widgets.central_arc, LV_OBJ_FLAG_CLICKABLE);

    /* Central label box */
    battery_widgets.central_label_box = lv_obj_create(battery_widgets.central_arc);
    lv_obj_set_size(battery_widgets.central_label_box, 25, 25);
    lv_obj_set_pos(battery_widgets.central_label_box, 0, 0);
    lv_obj_set_style_bg_opa(battery_widgets.central_label_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_widgets.central_label_box,
                              lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL), LV_PART_MAIN);
    lv_obj_set_style_radius(battery_widgets.central_label_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_widgets.central_label_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(battery_widgets.central_label_box, 0, LV_PART_MAIN);

    battery_widgets.central_label = lv_label_create(battery_widgets.central_label_box);
    lv_label_set_text(battery_widgets.central_label, "-");
    lv_obj_set_style_text_font(battery_widgets.central_label, &DINish_Medium_24, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(battery_widgets.central_label, -1, LV_PART_MAIN);
    lv_obj_set_style_text_color(battery_widgets.central_label,
                                lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_LABEL), LV_PART_MAIN);
    lv_obj_align(battery_widgets.central_label, LV_ALIGN_CENTER, 0, 0);

    /* Peripheral (right arc) */
    battery_widgets.peripheral_arc = lv_arc_create(battery_widgets.container);
    lv_obj_set_size(battery_widgets.peripheral_arc, arc_size, arc_size);
    lv_obj_set_pos(battery_widgets.peripheral_arc, spacing, y_center);
    lv_arc_set_range(battery_widgets.peripheral_arc, 0, 100);
    lv_arc_set_value(battery_widgets.peripheral_arc, 0);
    lv_arc_set_bg_angles(battery_widgets.peripheral_arc, 270, 180);
    lv_arc_set_rotation(battery_widgets.peripheral_arc, 0);
    lv_obj_set_style_arc_width(battery_widgets.peripheral_arc, ARC_WIDTH_DISCONNECTED, LV_PART_MAIN);
    lv_obj_set_style_arc_width(battery_widgets.peripheral_arc, ARC_WIDTH_DISCONNECTED, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(battery_widgets.peripheral_arc,
                               lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_RING), LV_PART_MAIN);
    lv_obj_set_style_arc_color(battery_widgets.peripheral_arc,
                               lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL), LV_PART_INDICATOR);
    lv_obj_remove_style(battery_widgets.peripheral_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(battery_widgets.peripheral_arc, LV_OBJ_FLAG_CLICKABLE);

    /* Peripheral label box */
    battery_widgets.peripheral_label_box = lv_obj_create(battery_widgets.peripheral_arc);
    lv_obj_set_size(battery_widgets.peripheral_label_box, 25, 25);
    lv_obj_set_pos(battery_widgets.peripheral_label_box, 0, 0);
    lv_obj_set_style_bg_opa(battery_widgets.peripheral_label_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_widgets.peripheral_label_box,
                              lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL), LV_PART_MAIN);
    lv_obj_set_style_radius(battery_widgets.peripheral_label_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(battery_widgets.peripheral_label_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(battery_widgets.peripheral_label_box, 0, LV_PART_MAIN);

    battery_widgets.peripheral_label = lv_label_create(battery_widgets.peripheral_label_box);
    lv_label_set_text(battery_widgets.peripheral_label, "-");
    lv_obj_set_style_text_font(battery_widgets.peripheral_label, &DINish_Medium_24, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(battery_widgets.peripheral_label, -1, LV_PART_MAIN);
    lv_obj_set_style_text_color(battery_widgets.peripheral_label,
                                lv_color_hex(DISPLAY_COLOR_BATTERY_DISCONNECTED_LABEL), LV_PART_MAIN);
    lv_obj_align(battery_widgets.peripheral_label, LV_ALIGN_CENTER, 0, 0);
}

static void update_battery_arc(lv_obj_t *arc, lv_obj_t *label_box, lv_obj_t *label,
                               uint8_t level, bool connected) {
    const operator_color_palette_t *p = &color_palettes[current_palette];
    bool low_battery = connected && level > 0 && level <= LOW_BATTERY_THRESHOLD;

    /* Update arc colors */
    uint32_t ring_color, fill_color;
    if (low_battery) {
        ring_color = p->battery_low_ring;
        fill_color = p->battery_low_fill;
    } else if (connected) {
        ring_color = p->battery_ring;
        fill_color = p->battery_fill;
    } else {
        ring_color = DISPLAY_COLOR_BATTERY_DISCONNECTED_RING;
        fill_color = DISPLAY_COLOR_BATTERY_DISCONNECTED_FILL;
    }

    lv_obj_set_style_arc_color(arc, lv_color_hex(ring_color), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(fill_color), LV_PART_INDICATOR);

    /* Update arc width and value */
    int arc_width = connected ? ARC_WIDTH_CONNECTED : ARC_WIDTH_DISCONNECTED;
    lv_obj_set_style_arc_width(arc, arc_width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, arc_width, LV_PART_INDICATOR);
    lv_arc_set_value(arc, connected ? level : 0);

    /* Update label box color */
    lv_obj_set_style_bg_color(label_box, lv_color_hex(fill_color), LV_PART_MAIN);

    /* Update label */
    char text[4];
    if (connected && level > 0) {
        snprintf(text, sizeof(text), "%d", level);
    } else {
        snprintf(text, sizeof(text), "-");
    }
    lv_label_set_text(label, text);

    /* Update label color */
    uint32_t label_color = connected ? 0x000000 : DISPLAY_COLOR_BATTERY_DISCONNECTED_LABEL;
    lv_obj_set_style_text_color(label, lv_color_hex(label_color), LV_PART_MAIN);
}

static void update_battery_circles(uint8_t central_level, bool central_connected,
                                   uint8_t peripheral_level, bool peripheral_connected) {
    update_battery_arc(battery_widgets.central_arc, battery_widgets.central_label_box,
                       battery_widgets.central_label, central_level, central_connected);
    update_battery_arc(battery_widgets.peripheral_arc, battery_widgets.peripheral_label_box,
                       battery_widgets.peripheral_label, peripheral_level, peripheral_connected);
}

/* ========== Output Indicator (USB/BLE) ========== */

/* BLE slot animation timer callback - handles both blink and fade */
static void ble_slot_anim_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    const operator_color_palette_t *p = &color_palettes[current_palette];

    if (ble_anim_state.active_slot >= 5 || !output_widgets.slot_boxes[ble_anim_state.active_slot]) {
        return;
    }

    lv_obj_t *slot = output_widgets.slot_boxes[ble_anim_state.active_slot];
    lv_obj_t *label = output_widgets.slot_labels[ble_anim_state.active_slot];

    switch (ble_anim_state.current_state) {
    case BLE_PROFILE_STATE_UNREGISTERED:
        /* Simple blink: toggle on/off */
        ble_anim_state.blink_phase = !ble_anim_state.blink_phase;
        if (ble_anim_state.blink_phase) {
            lv_obj_set_style_bg_color(slot, lv_color_hex(p->slot_active_bg), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(slot, lv_color_hex(p->slot_inactive_bg), LV_PART_MAIN);
        }
        break;

    case BLE_PROFILE_STATE_REGISTERED:
        /* Slow fade: smooth opacity transition */
        if (ble_anim_state.fade_direction) {
            /* Fading in */
            if (ble_anim_state.fade_value < 245) {
                ble_anim_state.fade_value += 10;
            } else {
                ble_anim_state.fade_value = 255;
                ble_anim_state.fade_direction = false;
            }
        } else {
            /* Fading out */
            if (ble_anim_state.fade_value > 80) {
                ble_anim_state.fade_value -= 10;
            } else {
                ble_anim_state.fade_value = 70;
                ble_anim_state.fade_direction = true;
            }
        }
        lv_obj_set_style_bg_color(slot, lv_color_hex(p->slot_active_bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(slot, ble_anim_state.fade_value, LV_PART_MAIN);
        lv_obj_set_style_text_opa(label, ble_anim_state.fade_value, LV_PART_MAIN);
        break;

    case BLE_PROFILE_STATE_CONNECTED:
        /* Should not be called - timer stopped for connected state */
        break;
    }
}

/* Start/stop animation timer based on connection state */
static void update_ble_slot_animation(uint8_t ble_profile, ble_profile_state_t new_state) {
    /* Check if state actually changed */
    bool state_changed = (new_state != ble_anim_state.current_state ||
                          ble_profile != ble_anim_state.active_slot);

    if (!state_changed) {
        return;
    }

    /* Stop existing timer */
    if (ble_anim_state.anim_timer != NULL) {
        lv_timer_del(ble_anim_state.anim_timer);
        ble_anim_state.anim_timer = NULL;
    }

    ble_anim_state.active_slot = ble_profile;
    ble_anim_state.current_state = new_state;

    /* Reset animation state */
    ble_anim_state.fade_value = 255;
    ble_anim_state.fade_direction = false;
    ble_anim_state.blink_phase = true;

    /* Create timer based on state */
    if (new_state == BLE_PROFILE_STATE_UNREGISTERED) {
        /* Blink timer: 400ms interval */
        ble_anim_state.anim_timer = lv_timer_create(ble_slot_anim_timer_cb, 400, NULL);
    } else if (new_state == BLE_PROFILE_STATE_REGISTERED) {
        /* Fade timer: 40ms interval */
        ble_anim_state.anim_timer = lv_timer_create(ble_slot_anim_timer_cb, 40, NULL);
    }

    /* Set initial state for connected (solid) */
    if (new_state == BLE_PROFILE_STATE_CONNECTED && ble_profile < 5) {
        const operator_color_palette_t *p = &color_palettes[current_palette];
        lv_obj_set_style_bg_color(output_widgets.slot_boxes[ble_profile],
                                  lv_color_hex(p->slot_active_bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(output_widgets.slot_boxes[ble_profile], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_opa(output_widgets.slot_labels[ble_profile], LV_OPA_COVER, LV_PART_MAIN);
    }
}

static void create_output_indicator(lv_obj_t *parent) {
    output_widgets.container = lv_obj_create(parent);
    lv_obj_set_size(output_widgets.container, 116, 62);  /* Original: 116x62 */
    lv_obj_set_pos(output_widgets.container, 148, 170);  /* Original: x=148, y=170 */
    lv_obj_set_style_bg_opa(output_widgets.container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(output_widgets.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(output_widgets.container, 0, LV_PART_MAIN);

    /* USB box - Original: 56x29 at (0, 0) with radius 6 */
    output_widgets.usb_box = lv_obj_create(output_widgets.container);
    lv_obj_set_size(output_widgets.usb_box, 56, 29);
    lv_obj_set_pos(output_widgets.usb_box, 0, 0);
    lv_obj_set_style_bg_opa(output_widgets.usb_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(output_widgets.usb_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(output_widgets.usb_box,
                                  lv_color_hex(DISPLAY_COLOR_USB_INACTIVE_BG), LV_PART_MAIN);
    lv_obj_set_style_radius(output_widgets.usb_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(output_widgets.usb_box, 0, LV_PART_MAIN);

    output_widgets.usb_label = lv_label_create(output_widgets.usb_box);
    lv_label_set_text(output_widgets.usb_label, "USB");
    lv_obj_set_style_text_font(output_widgets.usb_label, &FG_Medium_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(output_widgets.usb_label,
                                lv_color_hex(DISPLAY_COLOR_USB_INACTIVE_BG), LV_PART_MAIN);
    lv_obj_center(output_widgets.usb_label);
    lv_obj_set_style_translate_y(output_widgets.usb_label, 1, LV_PART_MAIN);

    /* BLE box - Original: 56x29 at (58, 0) with radius 6 */
    output_widgets.ble_box = lv_obj_create(output_widgets.container);
    lv_obj_set_size(output_widgets.ble_box, 56, 29);
    lv_obj_set_pos(output_widgets.ble_box, 58, 0);
    lv_obj_set_style_bg_opa(output_widgets.ble_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(output_widgets.ble_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(output_widgets.ble_box,
                                  lv_color_hex(DISPLAY_COLOR_BLE_INACTIVE_BG), LV_PART_MAIN);
    lv_obj_set_style_radius(output_widgets.ble_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(output_widgets.ble_box, 0, LV_PART_MAIN);

    output_widgets.ble_label = lv_label_create(output_widgets.ble_box);
    lv_label_set_text(output_widgets.ble_label, "BLE");
    lv_obj_set_style_text_font(output_widgets.ble_label, &FG_Medium_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(output_widgets.ble_label,
                                lv_color_hex(DISPLAY_COLOR_BLE_INACTIVE_BG), LV_PART_MAIN);
    lv_obj_center(output_widgets.ble_label);
    lv_obj_set_style_translate_y(output_widgets.ble_label, 1, LV_PART_MAIN);

    /* BLE profile slots (5 slots) - Original: dynamic width, y=33 */
    int slot_spacing = 2;
    int slot_width = (116 - (5 - 1) * slot_spacing) / 5;  /* = 21 for 5 profiles */
    int slot_y = 33;

    for (int i = 0; i < 5; i++) {
        output_widgets.slot_boxes[i] = lv_obj_create(output_widgets.container);
        lv_obj_set_size(output_widgets.slot_boxes[i], slot_width, 29);
        lv_obj_set_pos(output_widgets.slot_boxes[i], i * (slot_width + slot_spacing), slot_y);
        lv_obj_set_style_bg_color(output_widgets.slot_boxes[i],
                                  lv_color_hex(DISPLAY_COLOR_SLOT_INACTIVE_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(output_widgets.slot_boxes[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(output_widgets.slot_boxes[i], 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(output_widgets.slot_boxes[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(output_widgets.slot_boxes[i], 0, LV_PART_MAIN);

        output_widgets.slot_labels[i] = lv_label_create(output_widgets.slot_boxes[i]);
        char slot_text[2];
        snprintf(slot_text, sizeof(slot_text), "%d", i);  /* 0-based BLE profile numbering */
        lv_label_set_text(output_widgets.slot_labels[i], slot_text);
        lv_obj_set_style_text_font(output_widgets.slot_labels[i], &FG_Medium_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(output_widgets.slot_labels[i],
                                    lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_center(output_widgets.slot_labels[i]);
        lv_obj_set_style_translate_y(output_widgets.slot_labels[i], 1, LV_PART_MAIN);
    }
}

static void update_output_indicator(bool usb_connected, uint8_t ble_profile,
                                    bool ble_connected, bool ble_bonded) {
    const operator_color_palette_t *p = &color_palettes[current_palette];

    /* Update USB box - active: solid bg, inactive: border only */
    if (usb_connected) {
        lv_obj_set_style_bg_color(output_widgets.usb_box,
                                  lv_color_hex(p->usb_active_bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(output_widgets.usb_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(output_widgets.usb_box, 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(output_widgets.usb_label,
                                    lv_color_hex(p->output_active_text), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(output_widgets.usb_box, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(output_widgets.usb_box, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(output_widgets.usb_box,
                                      lv_color_hex(p->usb_inactive_bg), LV_PART_MAIN);
        lv_obj_set_style_text_color(output_widgets.usb_label,
                                    lv_color_hex(p->usb_inactive_bg), LV_PART_MAIN);
    }

    /* Update BLE box - active: solid bg, inactive: border only */
    bool ble_active = !usb_connected && ble_profile < 5;
    if (ble_active) {
        lv_obj_set_style_bg_color(output_widgets.ble_box,
                                  lv_color_hex(p->ble_active_bg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(output_widgets.ble_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(output_widgets.ble_box, 0, LV_PART_MAIN);
        lv_obj_set_style_text_color(output_widgets.ble_label,
                                    lv_color_hex(p->output_active_text), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(output_widgets.ble_box, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(output_widgets.ble_box, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(output_widgets.ble_box,
                                      lv_color_hex(p->ble_inactive_bg), LV_PART_MAIN);
        lv_obj_set_style_text_color(output_widgets.ble_label,
                                    lv_color_hex(p->ble_inactive_bg), LV_PART_MAIN);
    }

    /* Determine BLE profile connection state */
    ble_profile_state_t profile_state;
    if (ble_bonded && ble_connected) {
        profile_state = BLE_PROFILE_STATE_CONNECTED;
    } else if (ble_bonded) {
        profile_state = BLE_PROFILE_STATE_REGISTERED;
    } else {
        profile_state = BLE_PROFILE_STATE_UNREGISTERED;
    }

    /* Update slot boxes - non-active slots are always inactive color */
    for (int i = 0; i < 5; i++) {
        bool is_active = (i == ble_profile) && ble_active;
        if (!is_active) {
            /* Non-active slots: static inactive appearance */
            lv_obj_set_style_bg_color(output_widgets.slot_boxes[i],
                                      lv_color_hex(p->slot_inactive_bg), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(output_widgets.slot_boxes[i], LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_opa(output_widgets.slot_labels[i], LV_OPA_COVER, LV_PART_MAIN);
        }
    }

    /* Active slot: apply animation based on connection state */
    if (ble_active && ble_profile < 5) {
        /* Start/stop animation timer (only triggers on state change) */
        update_ble_slot_animation(ble_profile, profile_state);
    } else {
        /* USB mode or invalid profile: stop animation */
        update_ble_slot_animation(0, BLE_PROFILE_STATE_CONNECTED);
    }
}

/* ========== Public API ========== */

lv_obj_t *operator_layout_create(lv_obj_t *parent) {
    if (layout_container != NULL) {
        LOG_WRN("Operator layout already created");
        return layout_container;
    }

    /* Reset cached state to force initial update */
    memset(&cached_state, 0, sizeof(cached_state));

    /* Reset BLE animation state */
    ble_anim_state.anim_timer = NULL;
    ble_anim_state.active_slot = 0;
    ble_anim_state.current_state = BLE_PROFILE_STATE_CONNECTED;
    ble_anim_state.fade_value = 255;
    ble_anim_state.fade_direction = false;
    ble_anim_state.blink_phase = true;

    layout_container = lv_obj_create(parent);
    lv_obj_set_size(layout_container, 280, 240);  /* Software is landscape, mdac rotates to portrait */
    lv_obj_set_pos(layout_container, 0, 0);
    lv_obj_set_style_bg_color(layout_container, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layout_container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(layout_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(layout_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(layout_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Create all widgets */
    create_modifier_indicator(layout_container);
    create_wpm_meter(layout_container);
    create_layer_display(layout_container);
    create_battery_circles(layout_container);
    create_output_indicator(layout_container);

    LOG_INF("Operator layout created");
    return layout_container;
}

void operator_layout_update(uint8_t active_layer, const char *layer_name,
                           uint8_t battery_level, bool battery_connected,
                           uint8_t peripheral_battery, bool peripheral_connected,
                           uint8_t wpm, uint8_t modifier_flags,
                           bool usb_connected, uint8_t ble_profile,
                           bool ble_connected, bool ble_bonded) {
    if (layout_container == NULL) {
        return;
    }

    /* Only update modifiers if changed */
    if (!cached_state.initialized || modifier_flags != cached_state.modifier_flags) {
        update_modifier_indicator(modifier_flags);
        cached_state.modifier_flags = modifier_flags;
    }

    /* Only update WPM/layer if changed */
    if (!cached_state.initialized || wpm != cached_state.wpm ||
        !layer_name || strncmp(layer_name, cached_state.layer_name, sizeof(cached_state.layer_name)) != 0) {
        update_wpm_meter(wpm, layer_name);
        cached_state.wpm = wpm;
        if (layer_name) {
            strncpy(cached_state.layer_name, layer_name, sizeof(cached_state.layer_name) - 1);
        }
    }

    /* Only update layer display if changed */
    if (!cached_state.initialized || active_layer != cached_state.active_layer) {
        update_layer_display(active_layer);
        cached_state.active_layer = active_layer;
    }

    /* Only update battery if changed */
    if (!cached_state.initialized ||
        battery_level != cached_state.battery_level ||
        battery_connected != cached_state.battery_connected ||
        peripheral_battery != cached_state.peripheral_battery ||
        peripheral_connected != cached_state.peripheral_connected) {
        update_battery_circles(battery_level, battery_connected,
                              peripheral_battery, peripheral_connected);
        cached_state.battery_level = battery_level;
        cached_state.battery_connected = battery_connected;
        cached_state.peripheral_battery = peripheral_battery;
        cached_state.peripheral_connected = peripheral_connected;
    }

    /* Only update output indicator if changed */
    if (!cached_state.initialized ||
        usb_connected != cached_state.usb_connected ||
        ble_profile != cached_state.ble_profile ||
        ble_connected != cached_state.ble_connected ||
        ble_bonded != cached_state.ble_bonded) {
        update_output_indicator(usb_connected, ble_profile, ble_connected, ble_bonded);
        cached_state.usb_connected = usb_connected;
        cached_state.ble_profile = ble_profile;
        cached_state.ble_connected = ble_connected;
        cached_state.ble_bonded = ble_bonded;
    }

    cached_state.initialized = true;
}

void operator_layout_destroy(void) {
    /* Stop and delete animation timer */
    if (ble_anim_state.anim_timer != NULL) {
        lv_timer_del(ble_anim_state.anim_timer);
        ble_anim_state.anim_timer = NULL;
    }

    /* Reset cached state */
    memset(&cached_state, 0, sizeof(cached_state));

    /* Clear separator pointers */
    for (int i = 0; i < 3; i++) {
        mod_separators[i] = NULL;
    }

    if (layout_container != NULL) {
        lv_obj_del(layout_container);
        layout_container = NULL;
        LOG_INF("Operator layout destroyed");
    }
}

/* ========== Color Palette Functions ========== */

static void apply_palette(void) {
    if (layout_container == NULL) return;

    const operator_color_palette_t *p = &color_palettes[current_palette];

    /* Update modifier labels */
    for (int i = 0; i < 4; i++) {
        if (modifier_widgets.labels[i]) {
            lv_obj_set_style_text_color(modifier_widgets.labels[i],
                                        lv_color_hex(p->mod_inactive), LV_PART_MAIN);
        }
    }

    /* Update separators */
    for (int i = 0; i < 3; i++) {
        if (mod_separators[i]) {
            lv_obj_set_style_bg_color(mod_separators[i],
                                      lv_color_hex(p->mod_separator), LV_PART_MAIN);
        }
    }

    /* Update WPM bars (set to inactive color, will be updated properly on next data) */
    for (int i = 0; i < WPM_BAR_COUNT; i++) {
        if (wpm_widgets.bars[i]) {
            lv_obj_set_style_bg_color(wpm_widgets.bars[i],
                                      lv_color_hex(p->wpm_bar_inactive), LV_PART_MAIN);
        }
    }

    /* Update WPM label color */
    if (wpm_widgets.wpm_label) {
        lv_obj_set_style_text_color(wpm_widgets.wpm_label,
                                    lv_color_hex(p->wpm_text), LV_PART_MAIN);
    }

    /* Update layer label color */
    if (wpm_widgets.layer_label) {
        lv_obj_set_style_text_color(wpm_widgets.layer_label,
                                    lv_color_hex(p->layer_text), LV_PART_MAIN);
    }

    /* Update layer dots (set to inactive, will be updated on next data) */
    for (int i = 0; i < LAYER_DOT_MAX; i++) {
        if (layer_widgets.dots[i]) {
            lv_obj_set_style_bg_color(layer_widgets.dots[i],
                                      lv_color_hex(p->layer_dot_inactive), LV_PART_MAIN);
        }
    }

    /* Update USB/BLE boxes */
    if (output_widgets.usb_box) {
        lv_obj_set_style_bg_color(output_widgets.usb_box,
                                  lv_color_hex(p->usb_inactive_bg), LV_PART_MAIN);
        lv_obj_set_style_text_color(output_widgets.usb_label,
                                    lv_color_hex(p->output_inactive_text), LV_PART_MAIN);
    }

    if (output_widgets.ble_box) {
        lv_obj_set_style_bg_color(output_widgets.ble_box,
                                  lv_color_hex(p->ble_inactive_bg), LV_PART_MAIN);
        lv_obj_set_style_text_color(output_widgets.ble_label,
                                    lv_color_hex(p->output_inactive_text), LV_PART_MAIN);
    }

    /* Update slot boxes */
    for (int i = 0; i < 5; i++) {
        if (output_widgets.slot_boxes[i]) {
            lv_obj_set_style_bg_color(output_widgets.slot_boxes[i],
                                      lv_color_hex(p->slot_inactive_bg), LV_PART_MAIN);
        }
    }

    /* Save current state before invalidating */
    uint8_t saved_active_layer = cached_state.active_layer;
    uint8_t saved_wpm = cached_state.wpm;
    uint8_t saved_modifier_flags = cached_state.modifier_flags;
    uint8_t saved_battery_level = cached_state.battery_level;
    bool saved_battery_connected = cached_state.battery_connected;
    uint8_t saved_peripheral_battery = cached_state.peripheral_battery;
    bool saved_peripheral_connected = cached_state.peripheral_connected;
    bool saved_usb_connected = cached_state.usb_connected;
    uint8_t saved_ble_profile = cached_state.ble_profile;
    bool saved_ble_connected = cached_state.ble_connected;
    bool saved_ble_bonded = cached_state.ble_bonded;
    char saved_layer_name[8];
    strncpy(saved_layer_name, cached_state.layer_name, sizeof(saved_layer_name));

    /* Force cache invalidation */
    cached_state.initialized = false;

    /* Reset BLE animation state to force re-application */
    if (ble_anim_state.anim_timer != NULL) {
        lv_timer_del(ble_anim_state.anim_timer);
        ble_anim_state.anim_timer = NULL;
    }
    ble_anim_state.current_state = BLE_PROFILE_STATE_CONNECTED;  /* Force state change detection */
    ble_anim_state.active_slot = 255;  /* Invalid slot to force update */

    /* Re-apply current state with new colors */
    update_modifier_indicator(saved_modifier_flags);
    update_wpm_meter(saved_wpm, saved_layer_name);
    update_layer_display(saved_active_layer);
    update_battery_circles(saved_battery_level, saved_battery_connected,
                          saved_peripheral_battery, saved_peripheral_connected);
    update_output_indicator(saved_usb_connected, saved_ble_profile,
                           saved_ble_connected, saved_ble_bonded);

    /* Restore cache so we don't double-update on next data */
    cached_state.active_layer = saved_active_layer;
    cached_state.wpm = saved_wpm;
    cached_state.modifier_flags = saved_modifier_flags;
    cached_state.battery_level = saved_battery_level;
    cached_state.battery_connected = saved_battery_connected;
    cached_state.peripheral_battery = saved_peripheral_battery;
    cached_state.peripheral_connected = saved_peripheral_connected;
    cached_state.usb_connected = saved_usb_connected;
    cached_state.ble_profile = saved_ble_profile;
    cached_state.ble_connected = saved_ble_connected;
    cached_state.ble_bonded = saved_ble_bonded;
    strncpy(cached_state.layer_name, saved_layer_name, sizeof(cached_state.layer_name));
    cached_state.initialized = true;

    LOG_INF("Applied palette: %s", p->name);
}

void operator_layout_cycle_palette(void) {
    if (layout_container == NULL) return;

    current_palette = (current_palette + 1) % PALETTE_COUNT;
    apply_palette();
}

uint8_t operator_layout_get_palette(void) {
    return current_palette;
}

const char *operator_layout_get_palette_name(void) {
    return color_palettes[current_palette].name;
}
