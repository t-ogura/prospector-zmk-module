/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Scanner Pocket Display - Monochrome Memory LCD (LS013B7DH05)
 * 144x168 pixels, 1-bit color depth
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <zmk/status_scanner.h>

LOG_MODULE_REGISTER(pocket_display, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

/* ===== UI Components ===== */
static lv_obj_t *main_screen = NULL;
static lv_obj_t *device_name_label = NULL;
static lv_obj_t *layer_label = NULL;
static lv_obj_t *status_label = NULL;

/* ===== Scanner State ===== */
static char current_keyboard_name[32] = "Scanning...";
static uint8_t current_layer = 0;
static int8_t current_rssi = -100;

/* ===== Forward Declarations ===== */
static void start_scanner_delayed(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scanner_start_work, start_scanner_delayed);

/*
 * Scanner callback - receives keyboard status updates
 * Called from scanner thread, must not directly call LVGL APIs
 * WARNING: Direct LVGL calls here may cause thread-safety issues (see CLAUDE.md)
 */
static void scanner_update_callback(struct zmk_status_scanner_event_data *event_data) {
    if (!event_data || !event_data->status) {
        return;
    }

    struct zmk_keyboard_status *status = event_data->status;

    switch (event_data->event) {
    case ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND:
        LOG_INF("Keyboard found [%d]: %s", event_data->keyboard_index, status->ble_name);
        break;
    case ZMK_STATUS_SCANNER_EVENT_KEYBOARD_UPDATED:
        LOG_DBG("Keyboard updated [%d]: Layer %d, RSSI %d",
                event_data->keyboard_index,
                status->data.active_layer,
                status->rssi);
        break;
    case ZMK_STATUS_SCANNER_EVENT_KEYBOARD_LOST:
        LOG_INF("Keyboard lost [%d]: %s", event_data->keyboard_index, status->ble_name);
        break;
    default:
        return;
    }

    /* Store data for display update */
    strncpy(current_keyboard_name, status->ble_name, sizeof(current_keyboard_name) - 1);
    current_keyboard_name[sizeof(current_keyboard_name) - 1] = '\0';
    current_layer = status->data.active_layer;
    current_rssi = status->rssi;

    /*
     * TODO: Use ZMK event system for thread-safe LVGL updates
     * For now, direct update (may cause issues - see CLAUDE.md)
     * This is a temporary solution for initial testing only.
     */
    if (device_name_label) {
        lv_label_set_text(device_name_label, current_keyboard_name);
    }
    if (layer_label) {
        char layer_text[16];
        snprintf(layer_text, sizeof(layer_text), "Layer: %d", current_layer);
        lv_label_set_text(layer_label, layer_text);
    }
    if (status_label) {
        char status_text[32];
        snprintf(status_text, sizeof(status_text), "%ddBm", current_rssi);
        lv_label_set_text(status_label, status_text);
    }
}

/*
 * Delayed scanner initialization
 * Waits for display to be ready before starting BLE scan
 */
static void start_scanner_delayed(struct k_work *work) {
    if (!main_screen) {
        LOG_WRN("Display not ready, retrying...");
        k_work_schedule(&scanner_start_work, K_SECONDS(1));
        return;
    }

    LOG_INF("Starting BLE scanner...");

    /* Register callback to receive keyboard updates */
    int ret = zmk_status_scanner_register_callback(scanner_update_callback);
    if (ret < 0) {
        LOG_ERR("Failed to register scanner callback: %d", ret);
        if (status_label) {
            lv_label_set_text(status_label, "CB Error");
        }
        return;
    }

    /* Start scanning for keyboards */
    ret = zmk_status_scanner_start();
    if (ret < 0) {
        LOG_ERR("Failed to start scanner: %d", ret);
        if (status_label) {
            lv_label_set_text(status_label, "Scan Error");
        }
        return;
    }

    LOG_INF("Scanner started successfully");
    if (status_label) {
        lv_label_set_text(status_label, "Scanning...");
    }
}

/*
 * Create the main status screen
 * Required function for ZMK_DISPLAY_STATUS_SCREEN_CUSTOM
 *
 * Layout for 144x168 monochrome display:
 * ┌────────────────────────┐   0
 * │    Device Name         │  20 - Top: Keyboard name
 * ├────────────────────────┤  40
 * │                        │
 * │      Layer: 0          │  84 - Center: Layer display
 * │                        │
 * ├────────────────────────┤ 128
 * │       -45dBm           │ 148 - Bottom: RSSI
 * └────────────────────────┘ 168
 */
lv_obj_t *zmk_display_status_screen(void) {
    LOG_INF("Creating Scanner Pocket status screen (144x168 mono)");

    /* Create main screen with white background (Memory LCD is reflective) */
    main_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, LV_PART_MAIN);

    /* Device name label at top center */
    device_name_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(device_name_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_8, 0);
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(device_name_label, "Scanner Pocket");

    /* Layer label in center - large and prominent */
    layer_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(layer_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(layer_label, &lv_font_unscii_16, 0);
    lv_obj_align(layer_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(layer_label, "Layer: -");

    /* Status/RSSI label at bottom */
    status_label = lv_label_create(main_screen);
    lv_obj_set_style_text_color(status_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(status_label, &lv_font_unscii_8, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(status_label, "Starting...");

    /* Schedule scanner start after display is initialized */
    k_work_schedule(&scanner_start_work, K_MSEC(500));

    LOG_INF("Scanner Pocket screen created");
    return main_screen;
}

#endif /* CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY */
