/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Scanner Pocket Display - Monochrome Memory LCD (LS013B7DH05)
 * Landscape: 168x144 pixels, 1-bit color depth
 *
 * Layout based on original Prospector Scanner:
 * ┌────────────────────────────────────┐
 * │      Device Name                   │
 * │                                    │
 * │         Layer: 0                   │
 * │                                    │
 * │   [████] 85   [████] 42           │
 * │         RSSI: -45dBm              │
 * └────────────────────────────────────┘
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zmk/status_scanner.h>
#include <zmk/status_advertisement.h>
#include "custom_fonts.h"

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

LOG_MODULE_REGISTER(pocket_display, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

/* ===== Screen Dimensions ===== */
#if IS_ENABLED(CONFIG_SCANNER_POCKET_LANDSCAPE)
    #define SCREEN_W 168
    #define SCREEN_H 144
#else
    #define SCREEN_W 144
    #define SCREEN_H 168
#endif

/* ===== UI Components ===== */
static lv_obj_t *main_screen = NULL;
static lv_obj_t *device_name_label = NULL;

/* Layer widget - horizontal slide mode
 * Shows a row of layer numbers that slide horizontally when layer changes
 * Pattern: 5 visible slots with center slot highlighted
 */
#define SLIDE_VISIBLE_COUNT 5   /* Number of visible layer slots */
#define SLIDE_CENTER_SLOT 2     /* Index of center slot (0-based) */
#define SLIDE_SLOT_SPACING 20   /* Spacing between slots in pixels */

static lv_obj_t *layer_title = NULL;                              /* "Layer" title */
static lv_obj_t *layer_slide_labels[SLIDE_VISIBLE_COUNT] = {NULL}; /* Layer number labels */
static lv_obj_t *layer_indicator = NULL;                          /* Rectangle around active layer */
static int layer_slide_window_start = 0;                          /* First visible layer number */
static uint8_t current_layer = 0;

/* Modifier widget - NerdFont icons for Ctrl/Shift/Alt/GUI */
static lv_obj_t *modifier_label = NULL;
static uint8_t current_modifiers = 0;

/* BLE Profile widget - shows connection status with blinking
 * - Unpaired: fast blink (200ms)
 * - Paired but not connected: slow blink (500ms)
 * - Connected: solid (no blink)
 */
static lv_obj_t *ble_profile_label = NULL;
static uint8_t current_ble_profile = 0;
static bool current_usb_ready = false;
static bool current_ble_connected = false;
static bool current_ble_bonded = false;
static bool ble_blink_state = true;  /* true = visible */
static uint32_t ble_blink_counter = 0;

#define BLE_BLINK_FAST_PERIOD 2   /* 200ms (100ms timer * 2) */
#define BLE_BLINK_SLOW_PERIOD 5   /* 500ms (100ms timer * 5) */

/* WPM widget - typing speed display
 * Format: "WPM\n 42" (WPM on top, value below)
 */
static lv_obj_t *wpm_label = NULL;
static uint8_t current_wpm = 0;

/* Scanner Battery widget - scanner device's own battery
 * Shows at top-right with bar + percentage
 * Bar shape: rectangle with small protrusion on right (+ terminal)
 * Updates every 5 seconds
 */
static lv_obj_t *scanner_bat_bg = NULL;      /* Battery outline */
static lv_obj_t *scanner_bat_tip = NULL;     /* + terminal protrusion */
static lv_obj_t *scanner_bat_fill = NULL;    /* Fill level */
static lv_obj_t *scanner_bat_pct = NULL;     /* Percentage text */
static volatile int scanner_battery_level = 0;
static volatile bool scanner_is_charging = false;
static volatile bool scanner_battery_pending = false;
static bool scanner_charge_anim_running = false;

#define SCANNER_BATTERY_UPDATE_INTERVAL_MS 5000
#define SCANNER_BAT_WIDTH 24
#define SCANNER_BAT_HEIGHT 8
#define SCANNER_BAT_TIP_WIDTH 3
#define SCANNER_BAT_TIP_HEIGHT 4
#define SCANNER_BAT_FILL_MAX (SCANNER_BAT_WIDTH - 4)

/* NerdFont modifier symbols - From YADS project (MIT License) */
static const char *mod_symbols[4] = {
    "\xf3\xb0\x98\xb4",  /* 󰘴 Control (U+F0634) */
    "\xf3\xb0\x98\xb6",  /* 󰘶 Shift (U+F0636) */
    "\xf3\xb0\x98\xb5",  /* 󰘵 Alt (U+F0635) */
    "\xf3\xb0\x98\xb3"   /* 󰘳 GUI/Win/Cmd (U+F0633) */
};

/* Charging animation callback - animates fill width */
static void scanner_charge_anim_cb(void *var, int32_t value) {
    lv_obj_t *obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_width(obj, value);
    }
}

/* Called when charging animation completes - wait then restart */
static void scanner_charge_anim_ready_cb(lv_anim_t *anim) {
    /* Restart animation if still charging (with 1 second delay at current level) */
    if (scanner_is_charging && scanner_bat_fill && scanner_charge_anim_running) {
        int current_fill = (scanner_battery_level * SCANNER_BAT_FILL_MAX) / 100;
        if (current_fill < 1 && scanner_battery_level > 0) current_fill = 1;

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, scanner_bat_fill);
        lv_anim_set_exec_cb(&a, scanner_charge_anim_cb);
        lv_anim_set_values(&a, current_fill, SCANNER_BAT_FILL_MAX);
        lv_anim_set_time(&a, 4000);  /* 4 seconds to fill */
        lv_anim_set_delay(&a, 1000); /* 1 second wait at current level */
        lv_anim_set_ready_cb(&a, scanner_charge_anim_ready_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }
}

/* Start charging animation */
static void scanner_start_charge_anim(void) {
    if (!scanner_bat_fill || scanner_charge_anim_running) return;
    scanner_charge_anim_running = true;

    int current_fill = (scanner_battery_level * SCANNER_BAT_FILL_MAX) / 100;
    if (current_fill < 1 && scanner_battery_level > 0) current_fill = 1;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, scanner_bat_fill);
    lv_anim_set_exec_cb(&a, scanner_charge_anim_cb);
    lv_anim_set_values(&a, current_fill, SCANNER_BAT_FILL_MAX);
    lv_anim_set_time(&a, 4000);  /* 4 seconds to fill */
    lv_anim_set_ready_cb(&a, scanner_charge_anim_ready_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* Stop charging animation */
static void scanner_stop_charge_anim(void) {
    if (!scanner_bat_fill) return;
    scanner_charge_anim_running = false;
    lv_anim_del(scanner_bat_fill, scanner_charge_anim_cb);

    /* Set to actual level */
    int fill_w = (scanner_battery_level * SCANNER_BAT_FILL_MAX) / 100;
    if (fill_w < 1 && scanner_battery_level > 0) fill_w = 1;
    lv_obj_set_width(scanner_bat_fill, fill_w);
}

/* Scanner battery work queue - updates every 5 seconds
 * Only reads battery level and sets pending flag.
 * Actual LVGL update happens in display_timer_callback (main thread).
 */
static void scanner_battery_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scanner_battery_work, scanner_battery_work_handler);

static void scanner_battery_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    int level = zmk_battery_state_of_charge();
    if (level > 0) {
        scanner_battery_level = level;
    }
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    /* Check if USB is connected (= charging) */
    scanner_is_charging = zmk_usb_is_powered();
#endif

    /* Set pending flag for LVGL timer to process in main thread */
    scanner_battery_pending = true;

    /* Schedule next update */
    k_work_schedule(&scanner_battery_work, K_MSEC(SCANNER_BATTERY_UPDATE_INTERVAL_MS));
}

/* Update scanner battery display - called from LVGL timer (main thread) */
static void update_scanner_battery(void) {
    if (!scanner_bat_fill || !scanner_battery_pending) return;
    scanner_battery_pending = false;

    /* Update percentage text */
    if (scanner_bat_pct) {
        static char pct_text[8];
        snprintf(pct_text, sizeof(pct_text), "%d", scanner_battery_level);
        lv_label_set_text(scanner_bat_pct, pct_text);
    }

    /* Handle charging animation */
    if (scanner_is_charging) {
        /* Start animation if not already running */
        if (!scanner_charge_anim_running) {
            scanner_start_charge_anim();
        }
    } else {
        /* Stop animation and show actual level */
        if (scanner_charge_anim_running) {
            scanner_stop_charge_anim();
        } else {
            /* Just update fill bar width based on level */
            int fill_w = (scanner_battery_level * SCANNER_BAT_FILL_MAX) / 100;
            if (fill_w < 1 && scanner_battery_level > 0) fill_w = 1;
            lv_obj_set_width(scanner_bat_fill, fill_w);
        }
    }
}

/* Battery data storage (4 for Central/L/R/Aux) */
#define MAX_BATTERY_DATA 4

/* Battery display - Prospector Scanner layout with labels
 * Layout per slot:
 *   L 85     <- Label + Percentage (same line)
 *   [████]   <- Bar (bottom)
 * All elements use absolute positioning on parent screen
 */
#define MAX_BATTERY_WIDGETS 4
#define BAT_CONTAINER_HEIGHT 18
#define BAT_BAR_HEIGHT 5

static lv_obj_t *bat_bg[MAX_BATTERY_WIDGETS] = {NULL};      /* Bar background (border) */
static lv_obj_t *bat_fill[MAX_BATTERY_WIDGETS] = {NULL};    /* Bar fill */
static lv_obj_t *bat_pct[MAX_BATTERY_WIDGETS] = {NULL};     /* Percentage label */
static lv_obj_t *bat_name[MAX_BATTERY_WIDGETS] = {NULL};    /* Name label (L/R/Aux/A1/A2) */
static int16_t bat_bar_width[MAX_BATTERY_WIDGETS] = {0};    /* Stored bar width for updates */

static int active_battery_count = 0;

/* Color scheme */
static lv_color_t bg_color;
static lv_color_t text_color;

/* ===== Pending Display Data ===== */
static struct {
    volatile bool update_pending;
    char keyboard_name[32];
    uint8_t layer;
    uint8_t modifiers;
    uint8_t wpm;
    uint8_t ble_profile;
    bool usb_ready;
    bool ble_connected;
    bool ble_bonded;
    int8_t rssi;
    uint8_t bat[MAX_BATTERY_DATA];
    uint32_t callback_count;
} pending_data = {
    .update_pending = false,
    .keyboard_name = "Scanning...",
    .layer = 0,
    .modifiers = 0,
    .wpm = 0,
    .ble_profile = 0,
    .usb_ready = false,
    .ble_connected = false,
    .ble_bonded = false,
    .rssi = -100,
    .bat = {0, 0, 0, 0},
    .callback_count = 0,
};

static lv_timer_t *display_timer = NULL;

/* ===== Work Queue ===== */
static void display_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(display_work, display_work_handler);
static volatile bool display_work_pending = false;

static struct {
    struct k_mutex mutex;
    bool valid;
    char name[32];
    uint8_t layer;
    uint8_t modifiers;
    uint8_t wpm;
    uint8_t ble_profile;
    bool usb_ready;
    bool ble_connected;
    bool ble_bonded;
    int8_t rssi;
    uint8_t bat_central;
    uint8_t bat_left;
    uint8_t bat_right;
    uint8_t bat_aux;
} scanner_data = {0};
static bool scanner_data_mutex_init = false;

/* Scanner callback */
static void scanner_update_callback(struct zmk_status_scanner_event_data *event_data) {
    if (!event_data || !event_data->status) {
        return;
    }

    struct zmk_keyboard_status *status = event_data->status;

    if (!scanner_data_mutex_init) {
        k_mutex_init(&scanner_data.mutex);
        scanner_data_mutex_init = true;
    }

    if (k_mutex_lock(&scanner_data.mutex, K_MSEC(5)) != 0) {
        return;
    }

    strncpy(scanner_data.name, status->ble_name, sizeof(scanner_data.name) - 1);
    scanner_data.name[sizeof(scanner_data.name) - 1] = '\0';
    scanner_data.layer = status->data.active_layer;
    scanner_data.modifiers = status->data.modifier_flags;
    scanner_data.wpm = status->data.wpm_value;
    scanner_data.ble_profile = status->data.profile_slot;
    scanner_data.usb_ready = (status->data.status_flags & ZMK_STATUS_FLAG_USB_HID_READY) != 0;
    scanner_data.ble_connected = (status->data.status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED) != 0;
    scanner_data.ble_bonded = (status->data.status_flags & ZMK_STATUS_FLAG_BLE_BONDED) != 0;
    scanner_data.rssi = status->rssi;
    scanner_data.bat_central = status->data.battery_level;
    scanner_data.bat_left = status->data.peripheral_battery[0];
    scanner_data.bat_right = status->data.peripheral_battery[1];
    scanner_data.bat_aux = status->data.peripheral_battery[2];
    scanner_data.valid = true;

    k_mutex_unlock(&scanner_data.mutex);

    if (!display_work_pending) {
        display_work_pending = true;
        k_work_schedule(&display_work, K_MSEC(50));
    }
}

/* Work handler */
static void display_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    display_work_pending = false;

    if (!scanner_data_mutex_init) return;
    if (k_mutex_lock(&scanner_data.mutex, K_MSEC(10)) != 0) return;
    if (!scanner_data.valid) {
        k_mutex_unlock(&scanner_data.mutex);
        return;
    }

    strncpy(pending_data.keyboard_name, scanner_data.name, sizeof(pending_data.keyboard_name) - 1);
    pending_data.keyboard_name[sizeof(pending_data.keyboard_name) - 1] = '\0';
    pending_data.layer = scanner_data.layer;
    pending_data.modifiers = scanner_data.modifiers;
    pending_data.wpm = scanner_data.wpm;
    pending_data.ble_profile = scanner_data.ble_profile;
    pending_data.usb_ready = scanner_data.usb_ready;
    pending_data.ble_connected = scanner_data.ble_connected;
    pending_data.ble_bonded = scanner_data.ble_bonded;
    pending_data.rssi = scanner_data.rssi;
    pending_data.bat[0] = scanner_data.bat_central;
    pending_data.bat[1] = scanner_data.bat_left;
    pending_data.bat[2] = scanner_data.bat_right;
    pending_data.bat[3] = scanner_data.bat_aux;
    pending_data.callback_count++;

    k_mutex_unlock(&scanner_data.mutex);
    pending_data.update_pending = true;
}

/* ===== Layer Widget Functions ===== */

/* Animation callback for horizontal slide */
static void layer_slide_anim_cb(void *var, int32_t value) {
    lv_obj_t *obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_style_translate_x(obj, value, 0);
    }
}

/* Get X position for layer slot */
static int16_t get_layer_slot_x(int slot) {
    int16_t total_w = (SLIDE_VISIBLE_COUNT - 1) * SLIDE_SLOT_SPACING;
    int16_t start_x = (SCREEN_W - total_w) / 2;
    return start_x + slot * SLIDE_SLOT_SPACING - 8;  /* -8 for centering 16px wide number */
}

/* Update layer slide display - called when layer changes */
static void update_layer_indicator(uint8_t layer) {
    if (layer == current_layer) return;

    /* Calculate which slot the new layer should be in */
    int current_slot = layer - layer_slide_window_start;

    /* Check if we need to scroll the window */
    bool need_scroll = false;
    int new_window_start = layer_slide_window_start;

    if (current_slot < 0) {
        /* Layer is before visible window - scroll left */
        new_window_start = layer;
        need_scroll = true;
    } else if (current_slot >= SLIDE_VISIBLE_COUNT) {
        /* Layer is after visible window - scroll right */
        new_window_start = layer - SLIDE_VISIBLE_COUNT + 1;
        need_scroll = true;
    }

    /* Calculate scroll amount */
    int scroll_slots = new_window_start - layer_slide_window_start;

    /* Update window start */
    if (need_scroll) {
        layer_slide_window_start = new_window_start;
    }

    current_layer = layer;

    /* Update all label texts and positions */
    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        if (!layer_slide_labels[i]) continue;

        int layer_num = layer_slide_window_start + i;
        bool is_active = (layer_num == layer);

        /* Update text */
        char text[8];
        if (layer_num >= 0) {
            snprintf(text, sizeof(text), "%d", layer_num);
        } else {
            text[0] = '\0';
        }
        lv_label_set_text(layer_slide_labels[i], text);

        /* Update style - active layer is inverted */
        if (is_active) {
            lv_obj_set_style_bg_color(layer_slide_labels[i], text_color, 0);
            lv_obj_set_style_bg_opa(layer_slide_labels[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(layer_slide_labels[i], bg_color, 0);
        } else {
            lv_obj_set_style_bg_opa(layer_slide_labels[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(layer_slide_labels[i], text_color, 0);
        }

        /* Reset translate for fresh animation */
        lv_obj_set_style_translate_x(layer_slide_labels[i], 0, 0);
    }

    /* Animate scroll if needed */
    if (need_scroll && scroll_slots != 0) {
        int scroll_px = scroll_slots * SLIDE_SLOT_SPACING;

        for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
            if (!layer_slide_labels[i]) continue;

            lv_anim_t anim;
            lv_anim_init(&anim);
            lv_anim_set_var(&anim, layer_slide_labels[i]);
            lv_anim_set_exec_cb(&anim, layer_slide_anim_cb);
            lv_anim_set_values(&anim, scroll_px, 0);
            lv_anim_set_time(&anim, 150);
            lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
            lv_anim_start(&anim);
        }
    }

    LOG_DBG("Layer update: layer=%d, window_start=%d, scroll=%d",
            layer, layer_slide_window_start, scroll_slots);
}

/* Create layer widget with horizontal slide mode
 * Layout:
 *   "Layer" label centered above
 *   Horizontal row of numbers that slide
 */
static void create_layer_widget(lv_obj_t *parent) {
    int16_t layer_y = 68;  /* Y position for layer numbers (shifted down for scanner battery) */

    /* Create "Layer" title centered above - using quinquefive font */
    layer_title = lv_label_create(parent);
    lv_obj_set_style_text_color(layer_title, text_color, 0);
    lv_obj_set_style_text_font(layer_title, &quinquefive_8, 0);
    lv_obj_align(layer_title, LV_ALIGN_TOP_MID, 0, 56);
    lv_label_set_text(layer_title, "Layer");

    /* Initialize window to start at layer 0 */
    layer_slide_window_start = 0;

    /* Create layer number labels */
    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        int layer_num = layer_slide_window_start + i;
        bool is_active = (layer_num == 0);

        layer_slide_labels[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(layer_slide_labels[i], &lv_font_unscii_16, 0);
        lv_obj_set_style_text_align(layer_slide_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(layer_slide_labels[i], 18);
        lv_obj_set_style_pad_left(layer_slide_labels[i], 2, 0);
        lv_obj_set_style_pad_right(layer_slide_labels[i], 2, 0);

        /* Position */
        int16_t x = get_layer_slot_x(i);
        lv_obj_set_pos(layer_slide_labels[i], x, layer_y);

        /* Set text */
        char text[8];
        snprintf(text, sizeof(text), "%d", layer_num);
        lv_label_set_text(layer_slide_labels[i], text);

        /* Style - active layer is inverted */
        if (is_active) {
            lv_obj_set_style_bg_color(layer_slide_labels[i], text_color, 0);
            lv_obj_set_style_bg_opa(layer_slide_labels[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(layer_slide_labels[i], bg_color, 0);
        } else {
            lv_obj_set_style_bg_opa(layer_slide_labels[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(layer_slide_labels[i], text_color, 0);
        }
    }

    LOG_INF("Layer slide widget created (%d slots)", SLIDE_VISIBLE_COUNT);
}

/* ===== Battery Widget Functions ===== */

/* Update single battery bar and label */
static void update_battery_bar(int slot, uint8_t level, const char *name) {
    if (slot >= MAX_BATTERY_WIDGETS) return;
    if (bat_bar_width[slot] <= 0) return;  /* Not initialized yet */

    /* Update name label */
    if (bat_name[slot] && name) {
        lv_label_set_text(bat_name[slot], name);
    }

    /* Update fill width using stored bar width */
    if (bat_fill[slot]) {
        int16_t fill_w = (level * (bat_bar_width[slot] - 2)) / 100;
        if (fill_w < 1 && level > 0) fill_w = 1;
        lv_obj_set_width(bat_fill[slot], fill_w);
    }

    /* Update percentage label and re-center */
    if (bat_pct[slot]) {
        char buf[8];
        if (level > 0) {
            snprintf(buf, sizeof(buf), "%d", level);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(bat_pct[slot], buf);
        /* Re-align to center after text change */
        if (bat_bg[slot]) {
            lv_obj_align_to(bat_pct[slot], bat_bg[slot], LV_ALIGN_OUT_TOP_MID, 0, -2);
        }
    }
}

/* Reposition and show/hide battery widgets based on count
 * Layout per cell:
 *   L        <- Name label left-aligned with bar
 *      85    <- Percentage centered on bar
 *   [████]   <- Bar
 * 1 battery: centered with wide bar
 * 2-4 batteries: equal parts
 */
static void set_battery_layout(int count) {
    if (count < 1 || count > MAX_BATTERY_WIDGETS) return;

    int16_t y = 118;  /* Below Layer/Mod widgets (shifted down for scanner battery) */
    int16_t total_w = SCREEN_W - 8;  /* 160px usable */
    int16_t gap = 6;
    int16_t cell_w;
    int16_t start_x;

    if (count == 1) {
        /* Single battery: wide bar in center */
        cell_w = 80;
        start_x = (SCREEN_W - cell_w) / 2;
    } else {
        /* Multiple batteries: divide evenly */
        cell_w = (total_w - (count - 1) * gap) / count;
        start_x = 4;
    }

    for (int i = 0; i < MAX_BATTERY_WIDGETS; i++) {
        bool visible = (i < count);

        if (visible) {
            int16_t cell_x = start_x + i * (cell_w + gap);
            int16_t bar_w = cell_w;
            int16_t bar_x = cell_x;
            int16_t bar_y = y + BAT_CONTAINER_HEIGHT - BAT_BAR_HEIGHT;
            int16_t label_y = y;

            bat_bar_width[i] = bar_w;

            /* Bar background - full cell width (create first for alignment reference) */
            if (bat_bg[i]) {
                lv_obj_set_size(bat_bg[i], bar_w, BAT_BAR_HEIGHT);
                lv_obj_set_pos(bat_bg[i], bar_x, bar_y);
                lv_obj_clear_flag(bat_bg[i], LV_OBJ_FLAG_HIDDEN);
            }
            /* Name label (L/R/A) - left aligned with bar */
            if (bat_name[i]) {
                lv_obj_set_pos(bat_name[i], bar_x, label_y);
                lv_obj_clear_flag(bat_name[i], LV_OBJ_FLAG_HIDDEN);
            }
            /* Percentage label - centered above bar using align_to */
            if (bat_pct[i] && bat_bg[i]) {
                lv_obj_align_to(bat_pct[i], bat_bg[i], LV_ALIGN_OUT_TOP_MID, 0, -2);
                lv_obj_clear_flag(bat_pct[i], LV_OBJ_FLAG_HIDDEN);
            }
            /* Bar fill */
            if (bat_fill[i]) {
                lv_obj_set_pos(bat_fill[i], bar_x + 1, bar_y + 1);
                lv_obj_set_height(bat_fill[i], BAT_BAR_HEIGHT - 2);
                lv_obj_clear_flag(bat_fill[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            /* Hide unused slots */
            if (bat_name[i]) lv_obj_add_flag(bat_name[i], LV_OBJ_FLAG_HIDDEN);
            if (bat_pct[i]) lv_obj_add_flag(bat_pct[i], LV_OBJ_FLAG_HIDDEN);
            if (bat_bg[i]) lv_obj_add_flag(bat_bg[i], LV_OBJ_FLAG_HIDDEN);
            if (bat_fill[i]) lv_obj_add_flag(bat_fill[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* Update battery displays with appropriate labels
 * Labels: 2=L/R, 3=L/R/Aux, 4=L/R/A1/A2
 */
static void update_batteries(uint8_t central, uint8_t left, uint8_t right, uint8_t aux) {
    /* Determine which batteries are active */
    bool has_left = (left > 0);
    bool has_right = (right > 0);
    bool has_central = (central > 0);
    bool has_aux = (aux > 0);

    int count = 0;

    /* 4 batteries: L, R, A1, A2 */
    if (has_left && has_right && has_central && has_aux) {
        update_battery_bar(0, left, "L");
        update_battery_bar(1, right, "R");
        update_battery_bar(2, central, "A1");
        update_battery_bar(3, aux, "A2");
        count = 4;
    }
    /* 3 batteries: L, R, A */
    else if (has_left && has_right && has_central) {
        update_battery_bar(0, left, "L");
        update_battery_bar(1, right, "R");
        update_battery_bar(2, central, "A");
        count = 3;
    }
    else if (has_left && has_right && has_aux) {
        update_battery_bar(0, left, "L");
        update_battery_bar(1, right, "R");
        update_battery_bar(2, aux, "A");
        count = 3;
    }
    /* 2 batteries: L, R */
    else if (has_left && has_right) {
        update_battery_bar(0, left, "L");
        update_battery_bar(1, right, "R");
        count = 2;
    }
    else if (has_left && has_central) {
        update_battery_bar(0, left, "L");
        update_battery_bar(1, central, "R");
        count = 2;
    }
    else if (has_right && has_central) {
        update_battery_bar(0, right, "L");
        update_battery_bar(1, central, "R");
        count = 2;
    }
    /* Single battery */
    else if (has_left) {
        update_battery_bar(0, left, "L");
        count = 1;
    }
    else if (has_right) {
        update_battery_bar(0, right, "R");
        count = 1;
    }
    else if (has_central) {
        update_battery_bar(0, central, "C");
        count = 1;
    }

    if (count != active_battery_count) {
        active_battery_count = count;
        set_battery_layout(count);
    }
}

/* Update modifier display */
static void update_modifiers(uint8_t mods) {
    if (mods == current_modifiers) return;
    current_modifiers = mods;

    if (!modifier_label) return;

    char mod_text[64] = "";
    int pos = 0;

    /* Build NerdFont icon string - YADS style */
    if (mods & (ZMK_MOD_FLAG_LCTL | ZMK_MOD_FLAG_RCTL)) {
        pos += snprintf(mod_text + pos, sizeof(mod_text) - pos, "%s", mod_symbols[0]);
    }
    if (mods & (ZMK_MOD_FLAG_LSFT | ZMK_MOD_FLAG_RSFT)) {
        pos += snprintf(mod_text + pos, sizeof(mod_text) - pos, "%s", mod_symbols[1]);
    }
    if (mods & (ZMK_MOD_FLAG_LALT | ZMK_MOD_FLAG_RALT)) {
        pos += snprintf(mod_text + pos, sizeof(mod_text) - pos, "%s", mod_symbols[2]);
    }
    if (mods & (ZMK_MOD_FLAG_LGUI | ZMK_MOD_FLAG_RGUI)) {
        pos += snprintf(mod_text + pos, sizeof(mod_text) - pos, "%s", mod_symbols[3]);
    }

    /* Empty string when no modifiers active */
    lv_label_set_text(modifier_label, mod_text);
}

/* Update WPM display */
static void update_wpm(uint8_t wpm) {
    if (wpm == current_wpm) return;
    current_wpm = wpm;

    if (!wpm_label) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "WPM\n%3d", wpm);
    lv_label_set_text(wpm_label, buf);
}

/* Update BLE profile display */
static void update_connection(bool usb_rdy, bool ble_conn, bool ble_bond, uint8_t profile) {
    current_usb_ready = usb_rdy;
    current_ble_connected = ble_conn;
    current_ble_bonded = ble_bond;
    current_ble_profile = profile;

    if (!ble_profile_label) return;

    if (usb_rdy) {
        /* USB connected - show USB only */
        lv_label_set_text(ble_profile_label, "USB");
        lv_obj_clear_flag(ble_profile_label, LV_OBJ_FLAG_HIDDEN);
        ble_blink_state = true;  /* Always visible for USB */
    } else {
        /* Show BLE with profile number on new line (right-aligned) */
        char buf[16];
        snprintf(buf, sizeof(buf), "BLE\n %d", profile);
        lv_label_set_text(ble_profile_label, buf);
        /* Blink state will be handled by timer */
    }
}

/* Handle BLE profile blinking in timer */
static void handle_ble_blink(void) {
    if (!ble_profile_label) return;

    /* USB is always solid */
    if (current_usb_ready) {
        lv_obj_clear_flag(ble_profile_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* BLE connected = solid */
    if (current_ble_connected) {
        lv_obj_clear_flag(ble_profile_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Determine blink period */
    int blink_period;
    if (current_ble_bonded) {
        /* Paired but not connected: slow blink */
        blink_period = BLE_BLINK_SLOW_PERIOD;
    } else {
        /* Unpaired: fast blink */
        blink_period = BLE_BLINK_FAST_PERIOD;
    }

    /* Toggle visibility */
    ble_blink_counter++;
    if (ble_blink_counter >= blink_period) {
        ble_blink_counter = 0;
        ble_blink_state = !ble_blink_state;
        if (ble_blink_state) {
            lv_obj_clear_flag(ble_profile_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ble_profile_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* LVGL timer callback */
static void display_timer_callback(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    /* Handle BLE blink animation (runs every timer tick) */
    handle_ble_blink();

    /* Update scanner battery display (thread-safe via pending flag) */
    update_scanner_battery();

    if (!pending_data.update_pending) {
        return;
    }
    pending_data.update_pending = false;

    if (device_name_label) {
        lv_label_set_text(device_name_label, pending_data.keyboard_name);
    }

    /* Update layer indicator */
    if (pending_data.layer != current_layer) {
        update_layer_indicator(pending_data.layer);
    }

    /* Update modifiers */
    update_modifiers(pending_data.modifiers);

    /* Update WPM */
    update_wpm(pending_data.wpm);

    /* Update BLE profile */
    update_connection(pending_data.usb_ready, pending_data.ble_connected,
                      pending_data.ble_bonded, pending_data.ble_profile);

    update_batteries(
        pending_data.bat[0],
        pending_data.bat[1],
        pending_data.bat[2],
        pending_data.bat[3]
    );
}

/* Delayed scanner start */
static void start_scanner_delayed(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scanner_start_work, start_scanner_delayed);

static void start_scanner_delayed(struct k_work *work) {
    ARG_UNUSED(work);

    if (!main_screen) {
        k_work_schedule(&scanner_start_work, K_SECONDS(1));
        return;
    }

    int ret = zmk_status_scanner_register_callback(scanner_update_callback);
    if (ret < 0) {
        LOG_ERR("Failed to register scanner callback: %d", ret);
        return;
    }

    ret = zmk_status_scanner_start();
    if (ret < 0) {
        LOG_ERR("Failed to start scanner: %d", ret);
        return;
    }

    LOG_INF("Scanner started successfully");
}

/* Create a single battery slot (position will be set by set_battery_layout)
 * Layout:
 *   L 85     <- Name label + Percentage (same line)
 *   [████]   <- Bar (bottom)
 */
static void create_battery_slot(lv_obj_t *parent, int slot) {
    if (slot >= MAX_BATTERY_WIDGETS) return;

    /* Name label (L/R/Aux etc) */
    bat_name[slot] = lv_label_create(parent);
    lv_obj_set_style_text_color(bat_name[slot], text_color, 0);
    lv_obj_set_style_text_font(bat_name[slot], &lv_font_unscii_8, 0);
    lv_label_set_text(bat_name[slot], "-");
    lv_obj_add_flag(bat_name[slot], LV_OBJ_FLAG_HIDDEN);

    /* Percentage label */
    bat_pct[slot] = lv_label_create(parent);
    lv_obj_set_style_text_color(bat_pct[slot], text_color, 0);
    lv_obj_set_style_text_font(bat_pct[slot], &lv_font_unscii_8, 0);
    lv_label_set_text(bat_pct[slot], "--");
    lv_obj_add_flag(bat_pct[slot], LV_OBJ_FLAG_HIDDEN);

    /* Bar background (border) */
    bat_bg[slot] = lv_obj_create(parent);
    lv_obj_set_style_bg_color(bat_bg[slot], bg_color, 0);
    lv_obj_set_style_bg_opa(bat_bg[slot], LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bat_bg[slot], text_color, 0);
    lv_obj_set_style_border_width(bat_bg[slot], 1, 0);
    lv_obj_set_style_radius(bat_bg[slot], 1, 0);
    lv_obj_set_style_pad_all(bat_bg[slot], 0, 0);
    lv_obj_clear_flag(bat_bg[slot], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bat_bg[slot], LV_OBJ_FLAG_HIDDEN);

    /* Bar fill */
    bat_fill[slot] = lv_obj_create(parent);
    lv_obj_set_size(bat_fill[slot], 0, BAT_BAR_HEIGHT - 2);
    lv_obj_set_style_bg_color(bat_fill[slot], text_color, 0);
    lv_obj_set_style_bg_opa(bat_fill[slot], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bat_fill[slot], 0, 0);
    lv_obj_set_style_radius(bat_fill[slot], 0, 0);
    lv_obj_set_style_pad_all(bat_fill[slot], 0, 0);
    lv_obj_clear_flag(bat_fill[slot], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bat_fill[slot], LV_OBJ_FLAG_HIDDEN);
}

/* Create battery widget slots (positions set dynamically by set_battery_layout) */
static void create_battery_widgets(lv_obj_t *parent) {
    for (int i = 0; i < MAX_BATTERY_WIDGETS; i++) {
        create_battery_slot(parent, i);
    }
    active_battery_count = 0;
    LOG_INF("Battery widgets created (%d slots)", MAX_BATTERY_WIDGETS);
}

/* Create main screen */
lv_obj_t *zmk_display_status_screen(void) {
    LOG_INF("Creating Scanner Pocket screen (%dx%d)", SCREEN_W, SCREEN_H);

#if IS_ENABLED(CONFIG_SCANNER_POCKET_INVERT_COLORS)
    bg_color = lv_color_black();
    text_color = lv_color_white();
#else
    bg_color = lv_color_white();
    text_color = lv_color_black();
#endif

    main_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_screen, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Scanner Battery at top-right - bar shape with + terminal protrusion
     * Layout: [██████████]▌ 85
     *         ^bar        ^tip ^percentage
     * Charging: fill animates from current level to max, then repeats
     */
    int16_t bat_x = SCREEN_W - 4 - 20 - SCANNER_BAT_WIDTH - SCANNER_BAT_TIP_WIDTH;  /* Right-aligned */
    int16_t bat_y = 2;

    /* Battery outline (main rectangle) */
    scanner_bat_bg = lv_obj_create(main_screen);
    lv_obj_set_size(scanner_bat_bg, SCANNER_BAT_WIDTH, SCANNER_BAT_HEIGHT);
    lv_obj_set_pos(scanner_bat_bg, bat_x, bat_y);
    lv_obj_set_style_bg_color(scanner_bat_bg, bg_color, 0);
    lv_obj_set_style_bg_opa(scanner_bat_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scanner_bat_bg, text_color, 0);
    lv_obj_set_style_border_width(scanner_bat_bg, 1, 0);
    lv_obj_set_style_radius(scanner_bat_bg, 1, 0);
    lv_obj_set_style_pad_all(scanner_bat_bg, 0, 0);
    lv_obj_clear_flag(scanner_bat_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* + terminal protrusion (small rectangle on right) */
    scanner_bat_tip = lv_obj_create(main_screen);
    lv_obj_set_size(scanner_bat_tip, SCANNER_BAT_TIP_WIDTH, SCANNER_BAT_TIP_HEIGHT);
    lv_obj_set_pos(scanner_bat_tip, bat_x + SCANNER_BAT_WIDTH, bat_y + (SCANNER_BAT_HEIGHT - SCANNER_BAT_TIP_HEIGHT) / 2);
    lv_obj_set_style_bg_color(scanner_bat_tip, text_color, 0);
    lv_obj_set_style_bg_opa(scanner_bat_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scanner_bat_tip, 0, 0);
    lv_obj_set_style_radius(scanner_bat_tip, 0, 0);
    lv_obj_set_style_pad_all(scanner_bat_tip, 0, 0);
    lv_obj_clear_flag(scanner_bat_tip, LV_OBJ_FLAG_SCROLLABLE);

    /* Fill bar (inside the outline) */
    scanner_bat_fill = lv_obj_create(main_screen);
    lv_obj_set_size(scanner_bat_fill, 0, SCANNER_BAT_HEIGHT - 4);  /* Initial: 0 width */
    lv_obj_set_pos(scanner_bat_fill, bat_x + 2, bat_y + 2);
    lv_obj_set_style_bg_color(scanner_bat_fill, text_color, 0);
    lv_obj_set_style_bg_opa(scanner_bat_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scanner_bat_fill, 0, 0);
    lv_obj_set_style_radius(scanner_bat_fill, 0, 0);
    lv_obj_set_style_pad_all(scanner_bat_fill, 0, 0);
    lv_obj_clear_flag(scanner_bat_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* Percentage text (right of battery) */
    scanner_bat_pct = lv_label_create(main_screen);
    lv_obj_set_style_text_color(scanner_bat_pct, text_color, 0);
    lv_obj_set_style_text_font(scanner_bat_pct, &lv_font_unscii_8, 0);
    lv_obj_set_pos(scanner_bat_pct, bat_x + SCANNER_BAT_WIDTH + SCANNER_BAT_TIP_WIDTH + 2, bat_y);
    lv_label_set_text(scanner_bat_pct, "?");

    LOG_INF("Scanner battery widget created (bar style with charging animation)");

    /* Device name - shifted down for scanner battery */
    device_name_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(device_name_label, text_color, 0);
    lv_obj_set_style_text_font(device_name_label, &unscii_14, 0);
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(device_name_label, "Scanner Pocket");

    /* WPM label - between device name and layer, left-aligned
     * Format: "WPM\n  0" (WPM on top, value below)
     */
    wpm_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(wpm_label, text_color, 0);
    lv_obj_set_style_text_font(wpm_label, &lv_font_unscii_8, 0);
    lv_obj_align(wpm_label, LV_ALIGN_TOP_LEFT, 4, 32);
    lv_label_set_text(wpm_label, "WPM\n  0");
    LOG_INF("WPM widget created");

    /* BLE Profile label - between device name and layer, right-aligned
     * Format: "BLE\n 0" (BLE on top, profile number below)
     */
    ble_profile_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(ble_profile_label, text_color, 0);
    lv_obj_set_style_text_font(ble_profile_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_align(ble_profile_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(ble_profile_label, LV_ALIGN_TOP_RIGHT, -4, 32);
    lv_label_set_text(ble_profile_label, "BLE\n 0");
    LOG_INF("BLE profile widget created");

    /* Layer widget - horizontal row of numbers with indicator */
    create_layer_widget(main_screen);

    /* Modifier widget - NerdFont icons for Ctrl/Shift/Alt/GUI */
    modifier_label = lv_label_create(main_screen);
    lv_obj_set_style_text_font(modifier_label, &NerdFonts_Regular_40, 0);
    lv_obj_set_style_text_color(modifier_label, text_color, 0);
    lv_obj_set_style_text_letter_space(modifier_label, 8, 0);  /* Space between icons */
    lv_label_set_text(modifier_label, "");  /* Empty initially */
    lv_obj_align(modifier_label, LV_ALIGN_TOP_MID, 0, 82);
    LOG_INF("Modifier widget created");

    /* Battery widgets */
    create_battery_widgets(main_screen);

    /* LVGL timer */
    display_timer = lv_timer_create(display_timer_callback, 100, NULL);

    /* Start scanner */
    k_work_schedule(&scanner_start_work, K_MSEC(500));

    /* Start scanner battery update work (5 second interval) */
    k_work_schedule(&scanner_battery_work, K_MSEC(1000));

    LOG_INF("Scanner Pocket screen created");
    return main_screen;
}

#endif /* CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY */
