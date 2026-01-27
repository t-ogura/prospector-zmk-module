/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Scanner Pocket Display - Monochrome Memory LCD (LS013B7DH05)
 * Landscape: 168x144 pixels, 1-bit color depth
 *
 * Thread Safety Design (same as Prospector Scanner):
 * 1. BLE callback → scanner_stub.c stores data → schedules work queue
 * 2. Work queue → sets pending_data + update_pending flag (NO LVGL calls!)
 * 3. LVGL timer (main thread) → checks flag → updates display (ONLY LVGL calls here!)
 *
 * CRITICAL: NEVER call LVGL APIs from scanner callback or work queue!
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zmk/status_scanner.h>

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

/* ===== UI Components (created in main thread only) ===== */
static lv_obj_t *main_screen = NULL;
static lv_obj_t *device_name_label = NULL;
static lv_obj_t *layer_label = NULL;
static lv_obj_t *status_label = NULL;

/* Battery widgets - Left and Right */
static lv_obj_t *bat_label_l = NULL;
static lv_obj_t *bat_outline_l = NULL;
static lv_obj_t *bat_fill_l = NULL;

static lv_obj_t *bat_label_r = NULL;
static lv_obj_t *bat_outline_r = NULL;
static lv_obj_t *bat_fill_r = NULL;

/* Color scheme (set during screen creation) */
static lv_color_t bg_color;
static lv_color_t text_color;

/* ===== Pending Display Data (thread-safe flag-based update) ===== */
/*
 * DESIGN: Work queue sets data + flag, LVGL timer in main thread processes it
 * This is the SAME pattern used in Prospector Scanner's scanner_stub.c
 */
static struct {
    volatile bool update_pending;
    char keyboard_name[32];
    uint8_t layer;
    int8_t rssi;
    uint8_t battery_central;
    uint8_t battery_left;
    uint8_t battery_right;
    uint32_t callback_count;
} pending_data = {
    .update_pending = false,
    .keyboard_name = "Scanning...",
    .layer = 0,
    .rssi = -100,
    .battery_central = 0,
    .battery_left = 0,
    .battery_right = 0,
    .callback_count = 0,
};

/* LVGL timer for periodic display updates */
static lv_timer_t *display_timer = NULL;

/* ===== Work Queue for Display Updates ===== */
/*
 * This work handler runs in system work queue context.
 * It sets pending_data and flags - NO LVGL CALLS!
 */
static void display_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(display_work, display_work_handler);
static volatile bool display_work_pending = false;

/* Latest data from scanner (set by callback, read by work handler) */
static struct {
    struct k_mutex mutex;
    bool valid;
    char name[32];
    uint8_t layer;
    int8_t rssi;
    uint8_t bat_central;
    uint8_t bat_left;
    uint8_t bat_right;
} scanner_data = {0};
static bool scanner_data_mutex_init = false;

/*
 * Scanner callback - receives keyboard status updates
 * Called from BLE thread - MUST NOT call LVGL APIs!
 *
 * Design: Store data → Schedule work queue → Work sets pending_data → Timer updates LVGL
 */
static void scanner_update_callback(struct zmk_status_scanner_event_data *event_data) {
    if (!event_data || !event_data->status) {
        return;
    }

    struct zmk_keyboard_status *status = event_data->status;

    /* Initialize mutex on first call */
    if (!scanner_data_mutex_init) {
        k_mutex_init(&scanner_data.mutex);
        scanner_data_mutex_init = true;
    }

    /* Store data with mutex protection */
    if (k_mutex_lock(&scanner_data.mutex, K_MSEC(5)) != 0) {
        LOG_WRN("Failed to lock scanner_data mutex");
        return;
    }

    strncpy(scanner_data.name, status->ble_name, sizeof(scanner_data.name) - 1);
    scanner_data.name[sizeof(scanner_data.name) - 1] = '\0';
    scanner_data.layer = status->data.active_layer;
    scanner_data.rssi = status->rssi;
    scanner_data.bat_central = status->data.battery_level;
    scanner_data.bat_left = status->data.peripheral_battery[0];
    scanner_data.bat_right = status->data.peripheral_battery[1];
    scanner_data.valid = true;

    k_mutex_unlock(&scanner_data.mutex);

    /* Schedule work queue to process data (do NOT call LVGL here!) */
    if (!display_work_pending) {
        display_work_pending = true;
        k_work_schedule(&display_work, K_MSEC(50));  /* Small delay to batch updates */
    }

    LOG_DBG("CB: %s L%d Bat:%d/%d/%d",
            status->ble_name, status->data.active_layer,
            status->data.battery_level,
            status->data.peripheral_battery[0],
            status->data.peripheral_battery[1]);
}

/*
 * Display work handler - runs in system work queue
 * Reads scanner_data and sets pending_data flags
 * DOES NOT CALL LVGL APIS!
 */
static void display_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    display_work_pending = false;

    if (!scanner_data_mutex_init) {
        return;
    }

    if (k_mutex_lock(&scanner_data.mutex, K_MSEC(10)) != 0) {
        return;
    }

    if (!scanner_data.valid) {
        k_mutex_unlock(&scanner_data.mutex);
        return;
    }

    /* Copy to pending_data (NO LVGL CALLS!) */
    strncpy(pending_data.keyboard_name, scanner_data.name, sizeof(pending_data.keyboard_name) - 1);
    pending_data.keyboard_name[sizeof(pending_data.keyboard_name) - 1] = '\0';
    pending_data.layer = scanner_data.layer;
    pending_data.rssi = scanner_data.rssi;
    pending_data.battery_central = scanner_data.bat_central;
    pending_data.battery_left = scanner_data.bat_left;
    pending_data.battery_right = scanner_data.bat_right;
    pending_data.callback_count++;

    k_mutex_unlock(&scanner_data.mutex);

    /* Set flag for LVGL timer to pick up */
    pending_data.update_pending = true;

    LOG_INF("Work: Pending update set - %s L%d",
            pending_data.keyboard_name, pending_data.layer);
}

/*
 * Update battery - label text + fill width
 */
static void update_battery(lv_obj_t *label, lv_obj_t *fill, uint8_t level, const char *prefix) {
    /* Update label */
    if (label) {
        if (level > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s%d%%", prefix, level);
            lv_label_set_text(label, buf);
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%s--", prefix);
            lv_label_set_text(label, buf);
        }
    }

    /* Update fill width */
    if (fill) {
        int fill_width = (level * 42) / 100;  /* Max inner width = 42 */
        if (fill_width < 0) fill_width = 0;
        if (fill_width > 42) fill_width = 42;
        lv_obj_set_width(fill, fill_width);
    }
}

/*
 * LVGL timer callback - runs in main thread
 * This is the ONLY place where LVGL APIs are called!
 */
static void display_timer_callback(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    static uint32_t tick_count = 0;
    tick_count++;

    /* Always update status to show we're alive */
    if (status_label) {
        char buf[40];
        if (pending_data.update_pending) {
            snprintf(buf, sizeof(buf), "RSSI:%d cb:%u",
                     pending_data.rssi, (unsigned int)pending_data.callback_count);
        } else {
            snprintf(buf, sizeof(buf), "Waiting... t:%u",
                     (unsigned int)(tick_count % 1000));
        }
        lv_label_set_text(status_label, buf);
    }

    /* Check if there's pending data */
    if (!pending_data.update_pending) {
        return;
    }

    pending_data.update_pending = false;

    LOG_INF("Timer: Updating display - %s L%d Bat:%d/%d/%d",
            pending_data.keyboard_name,
            pending_data.layer,
            pending_data.battery_central,
            pending_data.battery_left,
            pending_data.battery_right);

    /* Update device name */
    if (device_name_label) {
        lv_label_set_text(device_name_label, pending_data.keyboard_name);
    }

    /* Update layer */
    if (layer_label) {
        char layer_text[16];
        snprintf(layer_text, sizeof(layer_text), "Layer: %d", pending_data.layer);
        lv_label_set_text(layer_label, layer_text);
    }

    /* Update battery bars - L only for driver revert test */
    uint8_t bat_l = pending_data.battery_left;
    uint8_t bat_r = pending_data.battery_right;

    if (bat_l == 0 && bat_r == 0 && pending_data.battery_central > 0) {
        bat_l = pending_data.battery_central;
    }

    update_battery(bat_label_l, bat_fill_l, bat_l, "L:");
    /* update_battery(bat_label_r, bat_fill_r, bat_r, "R:"); */
}

/* Battery widget creation removed - using inline label creation for minimal test */

/*
 * Delayed scanner initialization
 * Runs after display is ready
 */
static void start_scanner_delayed(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scanner_start_work, start_scanner_delayed);

static void start_scanner_delayed(struct k_work *work) {
    ARG_UNUSED(work);

    if (!main_screen) {
        LOG_WRN("Display not ready, retrying...");
        k_work_schedule(&scanner_start_work, K_SECONDS(1));
        return;
    }

    LOG_INF("Starting BLE scanner...");

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

/*
 * Create the main status screen
 * Called once during display initialization (main thread)
 *
 * Landscape layout (168x144):
 * ┌────────────────────────────────────┐  0
 * │         Device Name                │  8
 * │                                    │
 * │          Layer: 0                  │ 50
 * │                                    │
 * │  L [████████] 85%  R [████] 42%   │ 90
 * │                                    │
 * │         RSSI: -45dBm              │ 130
 * └────────────────────────────────────┘ 144
 */
lv_obj_t *zmk_display_status_screen(void) {
    LOG_INF("=============================================");
    LOG_INF("=== Scanner Pocket Screen (%dx%d) ===", SCREEN_W, SCREEN_H);
    LOG_INF("=== Thread-safe design (NO LVGL in callbacks) ===");
    LOG_INF("=============================================");

    /* Set color scheme */
#if IS_ENABLED(CONFIG_SCANNER_POCKET_INVERT_COLORS)
    bg_color = lv_color_black();
    text_color = lv_color_white();
    LOG_INF("Color scheme: inverted (black bg, white text)");
#else
    bg_color = lv_color_white();
    text_color = lv_color_black();
    LOG_INF("Color scheme: normal (white bg, black text)");
#endif

    /* Create main screen */
    main_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_screen, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);
    LOG_INF("Main screen created");

    /* Device name at top */
    device_name_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(device_name_label, text_color, 0);
    lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_8, 0);
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(device_name_label, "Scanner Pocket");
    LOG_INF("Device name label created");

    /* Layer label */
    layer_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(layer_label, text_color, 0);
    lv_obj_set_style_text_font(layer_label, &lv_font_unscii_16, 0);
    lv_obj_align(layer_label, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(layer_label, "Layer: -");
    LOG_INF("Layer label created");

    /* TEST: label + one simple rectangle */
    int16_t bat_y = SCREEN_H / 2 + 20;

    /* Battery label */
    bat_label_l = lv_label_create(main_screen);
    lv_obj_set_style_text_color(bat_label_l, text_color, 0);
    lv_obj_set_style_text_font(bat_label_l, &lv_font_unscii_8, 0);
    lv_obj_set_pos(bat_label_l, 20, bat_y);
    lv_label_set_text(bat_label_l, "L:--");

    /* === Left Battery === */
    bat_outline_l = lv_obj_create(main_screen);
    lv_obj_set_size(bat_outline_l, 46, 8);
    lv_obj_set_pos(bat_outline_l, 50, bat_y);
    lv_obj_set_style_bg_opa(bat_outline_l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(bat_outline_l, text_color, 0);
    lv_obj_set_style_border_width(bat_outline_l, 1, 0);
    lv_obj_set_style_radius(bat_outline_l, 2, 0);
    lv_obj_set_style_pad_all(bat_outline_l, 0, 0);
    lv_obj_clear_flag(bat_outline_l, LV_OBJ_FLAG_SCROLLABLE);

    bat_fill_l = lv_obj_create(main_screen);
    lv_obj_set_size(bat_fill_l, 0, 4);
    lv_obj_set_pos(bat_fill_l, 52, bat_y + 2);
    lv_obj_set_style_bg_color(bat_fill_l, text_color, 0);
    lv_obj_set_style_bg_opa(bat_fill_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bat_fill_l, 0, 0);
    lv_obj_set_style_radius(bat_fill_l, 0, 0);
    lv_obj_set_style_pad_all(bat_fill_l, 0, 0);
    lv_obj_clear_flag(bat_fill_l, LV_OBJ_FLAG_SCROLLABLE);

    /* Right battery - disabled to test driver revert */
    LOG_INF("Battery L only (R disabled, testing driver revert)");

    /* Status label at bottom */
    status_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(status_label, text_color, 0);
    lv_obj_set_style_text_font(status_label, &lv_font_unscii_8, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(status_label, "Starting...");
    LOG_INF("Status label created");

    /* Create LVGL timer for display updates (runs in main thread) */
    display_timer = lv_timer_create(display_timer_callback, 100, NULL);
    if (!display_timer) {
        LOG_ERR("Failed to create display timer");
    } else {
        LOG_INF("Display timer created (100ms interval)");
    }

    /* Schedule scanner start after short delay */
    k_work_schedule(&scanner_start_work, K_MSEC(500));

    LOG_INF("Scanner Pocket screen creation complete");
    return main_screen;
}

#endif /* CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY */
