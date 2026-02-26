/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * FIELD Layout for Scanner Mode
 * Design and layout based on carrefinho's prospector-zmk-module:
 * https://github.com/carrefinho/prospector-zmk-module
 *
 * Core visual: 8x6 grid of animated line segments driven by WPM.
 * Uses Perlin-like noise for organic angle variation, time-based decay,
 * and draws via lv_draw_line() callback for efficiency (zero LVGL objects for lines).
 */

#include "field_layout.h"
#include "fonts_carrefinho.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

LOG_MODULE_REGISTER(field_layout, CONFIG_ZMK_LOG_LEVEL);

/* ========== Grid Configuration (from carrefinho) ========== */

#define GRID_COLS 8
#define GRID_ROWS 6
#define GRID_TOTAL (GRID_COLS * GRID_ROWS)  /* 48 */
#define GRID_SPACING 34
#define LINE_LENGTH 14

/* Grid cell center positions (pre-computed) */
static const int grid_cx[GRID_COLS] = {18, 52, 86, 120, 154, 188, 222, 256};
static const int grid_cy[GRID_ROWS] = {18, 52, 86, 120, 154, 188};

/* ========== Animation Parameters (from carrefinho's Kconfig) ========== */

#define ANIM_BASE_SPEED           0.004f
#define ANIM_WPM_SPEED_MULTIPLIER 0.072f
#define WPM_REFERENCE             70       /* WPM at which animation reaches max speed */
#define INTENSITY_DECAY_SEC       30       /* Seconds for lines to fade after typing stops */
#define FLOW_DECAY_SEC            300      /* Seconds for directions to settle after stop */

/* Noise and angle parameters */
#define NOISE_ANGLE_INFLUENCE     1.5f     /* Noise effect multiplier (×π) */
#define ANGLE_SMOOTHING_RATE      0.15f    /* Interpolation toward target */

/* Idle wobble */
#define WOBBLE_FREQUENCY          0.5f
#define WOBBLE_SPATIAL_COL        0.3f
#define WOBBLE_SPATIAL_ROW        0.2f
#define WOBBLE_AMPLITUDE          0.5f     /* Radians */

/* Length breathing */
#define LENGTH_BASE_IDLE          0.5f
#define LENGTH_BASE_ACTIVE        0.5f
#define LENGTH_BREATH_FREQ        0.3f
#define LENGTH_BREATH_COL         0.5f
#define LENGTH_BREATH_ROW         0.4f
#define LENGTH_BREATH_AMPLITUDE   0.15f
#define LENGTH_MIN                0.5f
#define LENGTH_MAX                1.0f

/* Opacity */
#define OPACITY_BASE_IDLE         0.3f
#define OPACITY_BASE_ACTIVE       0.5f
#define OPACITY_VARIATION_SCALE   0.3f
#define OPACITY_VARIATION_FREQ    0.2f
#define OPACITY_DECAY_FACTOR      0.15f
#define OPACITY_MIN_F             0.15f
#define OPACITY_MAX_F             0.7f
#define NOISE_SPATIAL_X_SCALE     0.5f
#define NOISE_SPATIAL_Y_SCALE     0.5f

/* Decay rates */
#define DECAY_RATE_FAST_RISE      0.15f
#define DECAY_RATE_SLOW_FALL      0.005f
#define DECAY_RATE_IDLE_FALL      0.02f

/* Timer periods */
#define TIMER_PERIOD_30HZ  33
#define TIMER_PERIOD_15HZ  66
#define TIMER_PERIOD_2HZ   500

/* ========== Fast Sin/Cos LUT (float, 256 entries) ========== */

#define LUT_SIZE 256
#define LUT_MASK 0xFF
#define TWO_PI 6.28318530718f
#define M_PI_F 3.14159265359f

static float sin_lut[LUT_SIZE];
static bool lut_initialized = false;

static void init_lut(void) {
    if (lut_initialized) return;
    for (int i = 0; i < LUT_SIZE; i++) {
        sin_lut[i] = sinf((float)i * TWO_PI / LUT_SIZE);
    }
    lut_initialized = true;
}

static inline float fast_sin(float x) {
    float normalized = x * (LUT_SIZE / TWO_PI);
    int index = ((int)normalized) & LUT_MASK;
    return sin_lut[index];
}

static inline float fast_cos(float x) {
    return fast_sin(x + M_PI_F / 2.0f);
}

static inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ========== Color Palettes ========== */

typedef struct {
    uint32_t line_color;
    uint32_t layer_text;
    uint32_t battery_text;
    uint32_t mod_active;
    uint32_t mod_inactive;
    uint32_t output_active;
    uint32_t output_inactive;
    uint32_t background;
    const char *name;
} field_color_palette_t;

static const field_color_palette_t color_palettes[4] = {
    /* White (default - original carrefinho) */
    {
        .line_color = 0xFFFFFF,
        .layer_text = 0xFFFFFF,
        .battery_text = 0xFFFFFF,
        .mod_active = 0xFFFFFF,
        .mod_inactive = 0x404040,
        .output_active = 0x00FFFF,
        .output_inactive = 0x454545,
        .background = 0x000000,
        .name = "White"
    },
    /* Cyan */
    {
        .line_color = 0x00FFFF,
        .layer_text = 0x00FFFF,
        .battery_text = 0x80FFFF,
        .mod_active = 0x00FFFF,
        .mod_inactive = 0x004040,
        .output_active = 0x00FF80,
        .output_inactive = 0x303030,
        .background = 0x000000,
        .name = "Cyan"
    },
    /* Amber */
    {
        .line_color = 0xFFBF00,
        .layer_text = 0xFFD060,
        .battery_text = 0xFFD060,
        .mod_active = 0xFFBF00,
        .mod_inactive = 0x504000,
        .output_active = 0xFF8040,
        .output_inactive = 0x303020,
        .background = 0x000000,
        .name = "Amber"
    },
    /* Green */
    {
        .line_color = 0x00FF60,
        .layer_text = 0x80FFA0,
        .battery_text = 0x80FFA0,
        .mod_active = 0x00FF60,
        .mod_inactive = 0x004020,
        .output_active = 0x60FF60,
        .output_inactive = 0x203020,
        .background = 0x000000,
        .name = "Green"
    }
};

#define PALETTE_COUNT 4
static uint8_t current_palette = 0;

/* ========== Per-Cell Animation State ========== */

static float smoothed_angles[GRID_TOTAL];    /* Current smoothed angle (radians) */
static float line_length_scale[GRID_TOTAL];  /* Length multiplier 0.5-1.0 */
static lv_opa_t line_opacity[GRID_TOTAL];    /* Opacity 0-255 */

/* Pre-computed endpoint dx/dy for each cell (updated each frame) */
static int16_t line_dx[GRID_TOTAL];
static int16_t line_dy[GRID_TOTAL];

/* Exclusion bitmask (bit = cell index row*COLS+col) */
static uint64_t excluded_cells = 0;

/* ========== Decay System (from carrefinho) ========== */

typedef struct {
    float current_value;
    float at_stop_value;
    float target_value;
} decay_param_t;

static decay_param_t flow_state = {0};
static decay_param_t intensity_state = {0};

/* Animation state */
static float lines_time = 0.0f;
static float idle_wobble_time = 0.0f;
static uint8_t current_wpm = 0;
static uint32_t last_wpm_active_time = 0;
static bool animation_started = false;
static uint32_t last_timer_period = TIMER_PERIOD_2HZ;

/* ========== Layout Widgets ========== */

static lv_obj_t *layout_container = NULL;
static lv_obj_t *line_canvas_obj = NULL;  /* Custom draw object for lines */
static lv_obj_t *layer_label = NULL;
static lv_obj_t *battery_label = NULL;
static lv_obj_t *output_label = NULL;
static lv_obj_t *mod_labels[4] = {NULL};
static lv_timer_t *anim_timer = NULL;
static bool layout_created = false;

/* Cached state */
static struct {
    uint8_t active_layer;
    char layer_name[8];
    uint8_t wpm;
    uint8_t modifier_flags;
    uint8_t battery_level;
    uint8_t peripheral_battery;
    bool usb_connected;
    uint8_t ble_profile;
    bool ble_connected;
    bool initialized;
} cached = {0};

/* ========== Decay Computation (from carrefinho) ========== */

static decay_param_t compute_decay(decay_param_t state, int wpm,
                                   uint32_t idle_ms, uint32_t decay_ms) {
    decay_param_t result = state;

    if (wpm > 0) {
        result.target_value = clampf((float)wpm / (float)WPM_REFERENCE, 0.0f, 1.0f);
        result.at_stop_value = result.current_value;
    } else if (idle_ms < decay_ms) {
        float progress = (float)idle_ms / (float)decay_ms;
        result.target_value = result.at_stop_value * (1.0f - progress);
    } else {
        result.target_value = 0.0f;
    }

    /* Adaptive smoothing rate */
    float rate;
    if (wpm > 0 && result.target_value > result.current_value) {
        rate = DECAY_RATE_FAST_RISE;
    } else if (wpm > 0) {
        rate = DECAY_RATE_SLOW_FALL;
    } else {
        rate = DECAY_RATE_IDLE_FALL;
    }

    result.current_value += (result.target_value - result.current_value) * rate;
    return result;
}

/* ========== Perlin-like Noise (from carrefinho) ========== */

static float lines_noise(float x, float y, float t) {
    float n1 = fast_sin(x * 0.007f + t * 0.15f) *
               fast_cos(y * 0.008f - t * 0.12f);
    float n2 = fast_sin(y * 0.006f + t * 0.1f + x * 0.005f);
    return (n1 + n2 * 0.7f) / 1.7f;
}

/* ========== Exclusion Zone ========== */

static void compute_exclusions(void) {
    excluded_cells = 0;

    /* Layer label: top-left, ~3 cols × 1 row */
    for (int c = 0; c < 3; c++) {
        excluded_cells |= (1ULL << (0 * GRID_COLS + c));
    }

    /* Output label: row 1, ~2 cols */
    for (int c = 0; c < 2; c++) {
        excluded_cells |= (1ULL << (1 * GRID_COLS + c));
    }

    /* Modifier labels: rightmost column, rows 2-5 */
    for (int r = 2; r < GRID_ROWS; r++) {
        excluded_cells |= (1ULL << (r * GRID_COLS + (GRID_COLS - 1)));
    }

    /* Battery label: bottom-left, ~3 cols */
    for (int c = 0; c < 3; c++) {
        excluded_cells |= (1ULL << ((GRID_ROWS - 1) * GRID_COLS + c));
    }
}

/* ========== Line Animation Update (from carrefinho) ========== */

static void lines_update(void) {
    uint32_t now = k_uptime_get_32();
    uint32_t idle_ms = now - last_wpm_active_time;

    /* Update decay parameters */
    flow_state = compute_decay(flow_state, current_wpm, idle_ms,
                               FLOW_DECAY_SEC * 1000);
    intensity_state = compute_decay(intensity_state, current_wpm, idle_ms,
                                    INTENSITY_DECAY_SEC * 1000);

    float flow = flow_state.current_value;
    float intensity = intensity_state.current_value;

    /* Advance time based on WPM */
    float speed = ANIM_BASE_SPEED +
                  clampf((float)current_wpm / WPM_REFERENCE, 0, 1) * ANIM_WPM_SPEED_MULTIPLIER;
    lines_time += speed * (1.0f / 30.0f) * 60.0f;

    /* Advance idle wobble (always runs) */
    idle_wobble_time += 0.02f;
    if (idle_wobble_time > 628.0f) idle_wobble_time -= 628.0f;

    /* Update each grid cell */
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int idx = r * GRID_COLS + c;
            float cx = (float)grid_cx[c];
            float cy = (float)grid_cy[r];

            /* --- Angle --- */
            float noise_val = lines_noise(cx, cy, lines_time);
            float base_angle = -M_PI_F / 4.0f;  /* -45° default direction */
            float active_offset = noise_val * M_PI_F * NOISE_ANGLE_INFLUENCE;
            float target_angle = base_angle + active_offset * flow;

            /* Smooth interpolation toward target */
            smoothed_angles[idx] += (target_angle - smoothed_angles[idx]) * ANGLE_SMOOTHING_RATE;

            /* Add idle wobble */
            float wobble = fast_sin(idle_wobble_time * WOBBLE_FREQUENCY +
                                    c * WOBBLE_SPATIAL_COL +
                                    r * WOBBLE_SPATIAL_ROW) * WOBBLE_AMPLITUDE;
            float final_angle = smoothed_angles[idx] + wobble;

            /* Compute endpoint offsets */
            float cos_a = fast_cos(final_angle);
            float sin_a = fast_sin(final_angle);

            /* --- Length scale (breathing) --- */
            float base_scale = LENGTH_BASE_IDLE + flow * LENGTH_BASE_ACTIVE;
            float breath = fast_sin(lines_time * LENGTH_BREATH_FREQ +
                                    c * LENGTH_BREATH_COL +
                                    r * LENGTH_BREATH_ROW) * LENGTH_BREATH_AMPLITUDE;
            float scale = clampf(base_scale + breath * flow, LENGTH_MIN, LENGTH_MAX);
            line_length_scale[idx] = scale;

            int16_t dx = (int16_t)(LINE_LENGTH * scale * cos_a);
            int16_t dy = (int16_t)(LINE_LENGTH * scale * sin_a);
            line_dx[idx] = dx;
            line_dy[idx] = dy;

            /* --- Opacity --- */
            float base_opa = OPACITY_BASE_IDLE + intensity * OPACITY_BASE_ACTIVE;
            float spatial_var = lines_noise(cx * NOISE_SPATIAL_X_SCALE,
                                            cy * NOISE_SPATIAL_Y_SCALE,
                                            lines_time * OPACITY_VARIATION_FREQ);
            spatial_var = (spatial_var + 1.0f) * 0.5f;
            float opa_variation = spatial_var * OPACITY_VARIATION_SCALE * intensity;
            float final_opa = base_opa + opa_variation - OPACITY_DECAY_FACTOR * intensity;
            final_opa = clampf(final_opa, OPACITY_MIN_F, OPACITY_MAX_F);

            line_opacity[idx] = (lv_opa_t)(final_opa * 255.0f);
        }
    }
}

/* ========== Draw Callback (from carrefinho) ========== */

static void draw_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    int obj_x1 = coords.x1;
    int obj_y1 = coords.y1;

    const field_color_palette_t *p = &color_palettes[current_palette];

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(p->line_color);
    line_dsc.width = 2;
    line_dsc.round_start = 0;
    line_dsc.round_end = 0;

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int idx = r * GRID_COLS + c;

            /* Skip excluded cells */
            if (excluded_cells & (1ULL << idx)) {
                continue;
            }

            int cx = grid_cx[c];
            int cy = grid_cy[r];

            line_dsc.opa = line_opacity[idx];

            int16_t dx = line_dx[idx];
            int16_t dy = line_dy[idx];

            line_dsc.p1.x = obj_x1 + cx - dx;
            line_dsc.p1.y = obj_y1 + cy - dy;
            line_dsc.p2.x = obj_x1 + cx + dx;
            line_dsc.p2.y = obj_y1 + cy + dy;

            lv_draw_line(layer, &line_dsc);
        }
    }
}

/* ========== Animation Timer ========== */

static void animation_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    if (!layout_created || !line_canvas_obj) return;

    uint32_t now = k_uptime_get_32();
    uint32_t idle_ms = now - last_wpm_active_time;

    /* Adaptive frame rate (3 stages from carrefinho) */
    uint32_t target_period;
    if (current_wpm > 0) {
        target_period = TIMER_PERIOD_30HZ;
    } else if (animation_started && idle_ms < (FLOW_DECAY_SEC * 1000)) {
        target_period = TIMER_PERIOD_15HZ;
    } else {
        target_period = TIMER_PERIOD_2HZ;
    }

    if (target_period != last_timer_period) {
        lv_timer_set_period(anim_timer, target_period);
        last_timer_period = target_period;
    }

    /* Update animation state */
    lines_update();

    /* Invalidate to trigger draw callback */
    lv_obj_invalidate(line_canvas_obj);
}

/* ========== Create Functions ========== */

static void create_labels(lv_obj_t *parent) {
    const field_color_palette_t *p = &color_palettes[current_palette];

    /* Layer name label (top-left) - FR_Regular_36 */
    layer_label = lv_label_create(parent);
    lv_obj_set_style_text_font(layer_label, &FR_Regular_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(layer_label, lv_color_hex(p->layer_text), LV_PART_MAIN);
    lv_obj_set_pos(layer_label, 9, 14);
    lv_label_set_text(layer_label, "BASE");

    /* Output indicator (BLE profile, below layer) - FG_Medium_26 */
    output_label = lv_label_create(parent);
    lv_obj_set_style_text_font(output_label, &FG_Medium_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(output_label, lv_color_hex(p->output_inactive), LV_PART_MAIN);
    lv_obj_set_pos(output_label, 9, 52);
    lv_label_set_text(output_label, "");

    /* Battery label (bottom-left) - FR_Regular_30 */
    battery_label = lv_label_create(parent);
    lv_obj_set_style_text_font(battery_label, &FR_Regular_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(p->battery_text), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(battery_label, -1, LV_PART_MAIN);
    lv_obj_align(battery_label, LV_ALIGN_BOTTOM_LEFT, 9, -20);
    lv_label_set_text(battery_label, "-/-");
}

static void create_modifiers(lv_obj_t *parent) {
    const field_color_palette_t *p = &color_palettes[current_palette];

    /* Mac-style modifier symbols on right side (column 7, rows 2-5) */
    /* Order matches Radii: CMD, OPT, CTRL, SHIFT */
    static const char *mod_symbols[] = {
        SYMBOL_COMMAND,   /* ⌘ GUI */
        SYMBOL_OPTION,    /* ⌥ ALT */
        SYMBOL_CONTROL,   /* ⌃ CTRL */
        SYMBOL_SHIFT      /* ⇧ SHIFT */
    };

    for (int i = 0; i < 4; i++) {
        int r = i + 2;
        int x = grid_cx[GRID_COLS - 1] - 13;
        int y = grid_cy[r] - 4;
        mod_labels[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(mod_labels[i], &Symbols_Semibold_32, LV_PART_MAIN);
        lv_obj_set_style_text_color(mod_labels[i], lv_color_hex(p->mod_inactive), LV_PART_MAIN);
        lv_obj_set_pos(mod_labels[i], x, y);
        lv_label_set_text(mod_labels[i], mod_symbols[i]);
    }
}

/* ========== Update Functions ========== */

static void update_layer(const char *layer_name) {
    if (!layer_label) return;
    const field_color_palette_t *p = &color_palettes[current_palette];

    if (layer_name && layer_name[0]) {
        static char upper[32];
        int i;
        for (i = 0; layer_name[i] && i < 31; i++) {
            upper[i] = (layer_name[i] >= 'a' && layer_name[i] <= 'z')
                        ? layer_name[i] - 32 : layer_name[i];
        }
        upper[i] = '\0';
        lv_label_set_text(layer_label, upper);
    } else {
        lv_label_set_text(layer_label, "BASE");
    }
    lv_obj_set_style_text_color(layer_label, lv_color_hex(p->layer_text), LV_PART_MAIN);
}

static void update_modifiers(uint8_t flags) {
    const field_color_palette_t *p = &color_palettes[current_palette];

    /* Order: CMD, OPT, CTRL, SHIFT (matches symbol order) */
    bool mods[4] = {
        (flags & 0x88) != 0,  /* GUI/CMD */
        (flags & 0x44) != 0,  /* ALT/OPT */
        (flags & 0x11) != 0,  /* CTRL */
        (flags & 0x22) != 0,  /* SHIFT */
    };

    for (int i = 0; i < 4; i++) {
        if (mod_labels[i]) {
            lv_obj_set_style_text_color(mod_labels[i],
                lv_color_hex(mods[i] ? p->mod_active : p->mod_inactive), LV_PART_MAIN);
        }
    }
}

static void update_battery(uint8_t central, bool central_ok,
                            uint8_t peripheral, bool peripheral_ok) {
    if (!battery_label) return;
    const field_color_palette_t *p = &color_palettes[current_palette];

    char buf[16];
    if (central_ok && peripheral_ok && peripheral > 0) {
        snprintf(buf, sizeof(buf), "%d/%d", central, peripheral);
    } else if (central_ok && central > 0) {
        snprintf(buf, sizeof(buf), "%d", central);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    lv_label_set_text(battery_label, buf);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(p->battery_text), LV_PART_MAIN);
}

static void update_output(bool usb_connected, uint8_t ble_profile, bool ble_connected) {
    if (!output_label) return;
    const field_color_palette_t *p = &color_palettes[current_palette];

    if (usb_connected) {
        lv_label_set_text(output_label, "USB");
        lv_obj_set_style_text_color(output_label, lv_color_hex(p->output_active), LV_PART_MAIN);
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "BLE %d", ble_profile);
        lv_label_set_text(output_label, buf);
        lv_obj_set_style_text_color(output_label,
            lv_color_hex(ble_connected ? p->output_active : p->output_inactive), LV_PART_MAIN);
    }
}

/* ========== Palette Application ========== */

static void apply_palette(void) {
    if (!layout_created) return;
    const field_color_palette_t *p = &color_palettes[current_palette];

    if (layout_container) {
        lv_obj_set_style_bg_color(layout_container,
            lv_color_hex(p->background), LV_PART_MAIN);
    }
    if (layer_label) {
        lv_obj_set_style_text_color(layer_label, lv_color_hex(p->layer_text), LV_PART_MAIN);
    }
    if (battery_label) {
        lv_obj_set_style_text_color(battery_label, lv_color_hex(p->battery_text), LV_PART_MAIN);
    }
    if (output_label) {
        lv_obj_set_style_text_color(output_label, lv_color_hex(p->output_inactive), LV_PART_MAIN);
    }
    for (int i = 0; i < 4; i++) {
        if (mod_labels[i]) {
            lv_obj_set_style_text_color(mod_labels[i], lv_color_hex(p->mod_inactive), LV_PART_MAIN);
        }
    }

    /* Line colors update on next draw callback */
    LOG_INF("FIELD: Applied palette %s", p->name);
}

/* ========== Public API ========== */

lv_obj_t *field_layout_create(lv_obj_t *parent) {
    if (layout_created) {
        LOG_WRN("FIELD layout already created");
        return layout_container;
    }

    init_lut();

    layout_container = lv_obj_create(parent);
    lv_obj_set_size(layout_container, 280, 240);
    lv_obj_set_pos(layout_container, 0, 0);
    lv_obj_set_style_bg_color(layout_container,
        lv_color_hex(color_palettes[current_palette].background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layout_container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(layout_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(layout_container, 0, LV_PART_MAIN);
    lv_obj_clear_flag(layout_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Reset all animation state */
    memset(&cached, 0, sizeof(cached));
    memset(smoothed_angles, 0, sizeof(smoothed_angles));
    memset(line_length_scale, 0, sizeof(line_length_scale));
    memset(line_opacity, 0, sizeof(line_opacity));
    memset(line_dx, 0, sizeof(line_dx));
    memset(line_dy, 0, sizeof(line_dy));
    flow_state = (decay_param_t){0};
    intensity_state = (decay_param_t){0};
    lines_time = 0.0f;
    idle_wobble_time = 0.0f;
    current_wpm = 0;
    last_wpm_active_time = k_uptime_get_32();
    animation_started = false;
    last_timer_period = TIMER_PERIOD_2HZ;

    /* Initialize smoothed angles to -45° */
    for (int i = 0; i < GRID_TOTAL; i++) {
        smoothed_angles[i] = -M_PI_F / 4.0f;
        line_length_scale[i] = LENGTH_BASE_IDLE;
        line_opacity[i] = (lv_opa_t)(OPACITY_BASE_IDLE * 255.0f);
    }

    /* Compute exclusion zones */
    compute_exclusions();

    /* Create custom draw object for lines (transparent, full size) */
    line_canvas_obj = lv_obj_create(layout_container);
    lv_obj_set_size(line_canvas_obj, 274, 206);
    lv_obj_align(line_canvas_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(line_canvas_obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(line_canvas_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(line_canvas_obj, 0, LV_PART_MAIN);
    lv_obj_clear_flag(line_canvas_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(line_canvas_obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* Create labels on top of lines */
    create_labels(layout_container);
    create_modifiers(layout_container);

    /* Start animation timer (begins at 2Hz idle) */
    anim_timer = lv_timer_create(animation_timer_cb, TIMER_PERIOD_2HZ, NULL);

    layout_created = true;
    LOG_INF("FIELD layout created (%s theme)", color_palettes[current_palette].name);
    return layout_container;
}

void field_layout_update(uint8_t active_layer, const char *layer_name,
                         uint8_t battery_level, bool battery_connected,
                         uint8_t peripheral_battery, bool peripheral_connected,
                         uint8_t wpm, uint8_t modifier_flags,
                         bool usb_connected, uint8_t ble_profile,
                         bool ble_connected, bool ble_bonded) {
    if (!layout_created) return;

    /* Update WPM for animation */
    current_wpm = wpm;
    if (wpm > 0) {
        last_wpm_active_time = k_uptime_get_32();
        animation_started = true;
    }

    /* Layer name */
    if (!cached.initialized || active_layer != cached.active_layer ||
        !layer_name || strncmp(layer_name, cached.layer_name, sizeof(cached.layer_name)) != 0) {
        update_layer(layer_name);
        cached.active_layer = active_layer;
        if (layer_name) {
            strncpy(cached.layer_name, layer_name, sizeof(cached.layer_name) - 1);
        }
    }

    /* Modifiers */
    if (!cached.initialized || modifier_flags != cached.modifier_flags) {
        update_modifiers(modifier_flags);
        cached.modifier_flags = modifier_flags;
    }

    /* Battery */
    if (!cached.initialized || battery_level != cached.battery_level ||
        peripheral_battery != cached.peripheral_battery) {
        update_battery(battery_level, battery_connected,
                       peripheral_battery, peripheral_connected);
        cached.battery_level = battery_level;
        cached.peripheral_battery = peripheral_battery;
    }

    /* Output */
    if (!cached.initialized || usb_connected != cached.usb_connected ||
        ble_profile != cached.ble_profile || ble_connected != cached.ble_connected) {
        update_output(usb_connected, ble_profile, ble_connected);
        cached.usb_connected = usb_connected;
        cached.ble_profile = ble_profile;
        cached.ble_connected = ble_connected;
    }

    cached.initialized = true;
}

void field_layout_destroy(void) {
    if (!layout_created) return;

    if (anim_timer) {
        lv_timer_del(anim_timer);
        anim_timer = NULL;
    }

    layer_label = NULL;
    battery_label = NULL;
    output_label = NULL;
    line_canvas_obj = NULL;
    for (int i = 0; i < 4; i++) mod_labels[i] = NULL;

    if (layout_container) {
        lv_obj_del(layout_container);
        layout_container = NULL;
    }

    memset(&cached, 0, sizeof(cached));
    layout_created = false;
    LOG_INF("FIELD layout destroyed");
}

void field_layout_cycle_palette(void) {
    if (!layout_created) return;
    current_palette = (current_palette + 1) % PALETTE_COUNT;
    apply_palette();
}

uint8_t field_layout_get_palette(void) {
    return current_palette;
}

const char *field_layout_get_palette_name(void) {
    return color_palettes[current_palette].name;
}
