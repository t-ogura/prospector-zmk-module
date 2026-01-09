/**
 * Prospector Scanner UI - Full Widget Test
 * NO CONTAINER PATTERN - All widgets use absolute positioning
 *
 * Screen: 280x240 (90 degree rotated from 240x280)
 *
 * Supports screen transitions via swipe gestures:
 * - Swipe DOWN: Main screen → Settings screen
 * - Swipe UP: Settings screen → Main screen
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include "events/swipe_gesture_event.h"
#include "fonts.h"  /* NerdFont declarations */

LOG_MODULE_REGISTER(display_screen, LOG_LEVEL_INF);

/* ========== Screen State Management ========== */
enum screen_state {
    SCREEN_MAIN = 0,
    SCREEN_SETTINGS,
};

static enum screen_state current_screen = SCREEN_MAIN;
static lv_obj_t *screen_obj = NULL;

/* Transition protection flag - checked by work queues */
volatile bool transition_in_progress = false;

/* Forward declarations */
static void destroy_main_screen_widgets(void);
static void create_main_screen_widgets(void);
static void destroy_settings_screen_widgets(void);
static void create_settings_screen_widgets(void);

/* Modifier flag definitions (from status_advertisement.h) */
#define ZMK_MOD_FLAG_LCTL    (1 << 0)
#define ZMK_MOD_FLAG_LSFT    (1 << 1)
#define ZMK_MOD_FLAG_LALT    (1 << 2)
#define ZMK_MOD_FLAG_LGUI    (1 << 3)
#define ZMK_MOD_FLAG_RCTL    (1 << 4)
#define ZMK_MOD_FLAG_RSFT    (1 << 5)
#define ZMK_MOD_FLAG_RALT    (1 << 6)
#define ZMK_MOD_FLAG_RGUI    (1 << 7)

/* Font declarations */
LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_unscii_8);
LV_FONT_DECLARE(lv_font_unscii_16);

/* NerdFont modifier symbols - From YADS project (MIT License) */
static const char *mod_symbols[4] = {
    "\xf3\xb0\x98\xb4",  /* 󰘴 Control (U+F0634) */
    "\xf3\xb0\x98\xb6",  /* 󰘶 Shift (U+F0636) */
    "\xf3\xb0\x98\xb5",  /* 󰘵 Alt (U+F0635) */
    "\xf3\xb0\x98\xb3"   /* 󰘳 GUI/Win/Cmd (U+F0633) */
};

/* ========== Cached data (updated by scanner) ========== */
static int active_layer = 0;
static int wpm_value = 0;
static int battery_left = 0;
static int battery_right = 0;
static int scanner_battery = 0;
static int rssi = 0;
static float rate_hz = 0.0f;
static int ble_profile = 0;
static bool usb_ready = false;
static bool ble_connected = false;

/* ========== Widget references (NO CONTAINERS) ========== */

/* Device name */
static lv_obj_t *device_name_label = NULL;

/* Scanner battery */
static lv_obj_t *scanner_bat_icon = NULL;
static lv_obj_t *scanner_bat_pct = NULL;

/* WPM */
static lv_obj_t *wpm_title_label = NULL;
static lv_obj_t *wpm_value_label = NULL;

/* Connection status */
static lv_obj_t *transport_label = NULL;
static lv_obj_t *ble_profile_label = NULL;

/* Layer */
static lv_obj_t *layer_title_label = NULL;
static lv_obj_t *layer_labels[10] = {NULL};

/* Modifier - placeholder for now (no NerdFont in ZMK test) */
static lv_obj_t *modifier_label = NULL;

/* Keyboard battery - connected state */
static lv_obj_t *left_bat_bar = NULL;
static lv_obj_t *left_bat_label = NULL;
static lv_obj_t *right_bat_bar = NULL;
static lv_obj_t *right_bat_label = NULL;

/* Keyboard battery - disconnected state (× symbol) */
static lv_obj_t *left_bat_nc_bar = NULL;
static lv_obj_t *left_bat_nc_label = NULL;
static lv_obj_t *right_bat_nc_bar = NULL;
static lv_obj_t *right_bat_nc_label = NULL;

/* Signal status */
static lv_obj_t *channel_label = NULL;
static lv_obj_t *rx_title_label = NULL;
static lv_obj_t *rssi_bar = NULL;
static lv_obj_t *rssi_label = NULL;
static lv_obj_t *rate_label = NULL;

/* ========== Settings Screen Widgets ========== */
static lv_obj_t *settings_title_label = NULL;
static lv_obj_t *settings_info_label = NULL;
static lv_obj_t *settings_back_hint = NULL;

/* ========== Color functions ========== */

static lv_color_t get_layer_color(int layer) {
    switch (layer) {
        case 0: return lv_color_make(0xFF, 0x9B, 0x9B);
        case 1: return lv_color_make(0xFF, 0xD9, 0x3D);
        case 2: return lv_color_make(0x6B, 0xCF, 0x7F);
        case 3: return lv_color_make(0x4D, 0x96, 0xFF);
        case 4: return lv_color_make(0xB1, 0x9C, 0xD9);
        case 5: return lv_color_make(0xFF, 0x6B, 0x9D);
        case 6: return lv_color_make(0xFF, 0x9F, 0x43);
        case 7: return lv_color_make(0x87, 0xCE, 0xEB);
        case 8: return lv_color_make(0xF0, 0xE6, 0x8C);
        case 9: return lv_color_make(0xDD, 0xA0, 0xDD);
        default: return lv_color_white();
    }
}

static lv_color_t get_scanner_battery_color(int level) {
    if (level >= 80) return lv_color_hex(0x00FF00);
    else if (level >= 60) return lv_color_hex(0x7FFF00);
    else if (level >= 40) return lv_color_hex(0xFFFF00);
    else if (level >= 20) return lv_color_hex(0xFF7F00);
    else return lv_color_hex(0xFF0000);
}

static lv_color_t get_keyboard_battery_color(int level) {
    if (level >= 80) return lv_color_hex(0x00CC66);
    else if (level >= 60) return lv_color_hex(0x66CC00);
    else if (level >= 40) return lv_color_hex(0xFFCC00);
    else if (level >= 20) return lv_color_hex(0xFF8800);
    else return lv_color_hex(0xFF3333);
}

static const char* get_battery_icon(int level) {
    if (level >= 80) return LV_SYMBOL_BATTERY_FULL;
    else if (level >= 60) return LV_SYMBOL_BATTERY_3;
    else if (level >= 40) return LV_SYMBOL_BATTERY_2;
    else if (level >= 20) return LV_SYMBOL_BATTERY_1;
    else return LV_SYMBOL_BATTERY_EMPTY;
}

static uint8_t rssi_to_bars(int8_t rssi_val) {
    if (rssi_val >= -50) return 5;
    if (rssi_val >= -60) return 4;
    if (rssi_val >= -70) return 3;
    if (rssi_val >= -80) return 2;
    if (rssi_val >= -90) return 1;
    return 0;
}

static lv_color_t get_rssi_color(uint8_t bars) {
    switch (bars) {
        case 5: return lv_color_make(0xC0, 0xC0, 0xC0);
        case 4: return lv_color_make(0xA0, 0xA0, 0xA0);
        case 3: return lv_color_make(0x80, 0x80, 0x80);
        case 2: return lv_color_make(0x60, 0x60, 0x60);
        case 1: return lv_color_make(0x40, 0x40, 0x40);
        default: return lv_color_make(0x20, 0x20, 0x20);
    }
}

/* ========== Main Screen Creation (NO CONTAINERS) ========== */

lv_obj_t *zmk_display_status_screen(void) {
    LOG_INF("=============================================");
    LOG_INF("=== Full Widget Test - NO CONTAINER ===");
    LOG_INF("=== All widgets use absolute positioning ===");
    LOG_INF("=============================================");

    /* Create main screen */
    LOG_INF("[INIT] Creating main_screen...");
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    LOG_INF("[INIT] main_screen created");

    /* ===== 1. Device Name (TOP_MID, y=25) ===== */
    LOG_INF("[INIT] Creating device name...");
    device_name_label = lv_label_create(screen);
    lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(device_name_label, lv_color_white(), 0);
    lv_label_set_text(device_name_label, "Scanning...");
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 25);
    LOG_INF("[INIT] device name created");

    /* ===== 2. Scanner Battery (TOP_RIGHT area) ===== */
    /* Shows scanner device's own battery level */
    LOG_INF("[INIT] Creating scanner battery...");
    scanner_bat_icon = lv_label_create(screen);
    lv_obj_set_style_text_font(scanner_bat_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(scanner_bat_icon, 216, 4);  /* 4px right */
    lv_label_set_text(scanner_bat_icon, LV_SYMBOL_BATTERY_3);  /* Initial: 3/4 battery */
    lv_obj_set_style_text_color(scanner_bat_icon, lv_color_hex(0x7FFF00), 0);  /* Lime green */

    scanner_bat_pct = lv_label_create(screen);
    lv_obj_set_style_text_font(scanner_bat_pct, &lv_font_unscii_8, 0);
    lv_obj_set_pos(scanner_bat_pct, 238, 7);  /* 2px up */
    lv_label_set_text(scanner_bat_pct, "?");  /* Unknown until battery read */
    lv_obj_set_style_text_color(scanner_bat_pct, lv_color_hex(0x7FFF00), 0);
    LOG_INF("[INIT] scanner battery created");

    /* ===== 3. WPM Widget (TOP_LEFT, centered under title) ===== */
    LOG_INF("[INIT] Creating WPM...");
    wpm_title_label = lv_label_create(screen);
    lv_obj_set_style_text_font(wpm_title_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(wpm_title_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(wpm_title_label, "WPM");
    lv_obj_set_pos(wpm_title_label, 20, 53);  /* 3px down */

    wpm_value_label = lv_label_create(screen);
    lv_obj_set_style_text_font(wpm_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(wpm_value_label, lv_color_white(), 0);
    lv_obj_set_width(wpm_value_label, 48);  /* Fixed width for centering */
    lv_obj_set_style_text_align(wpm_value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(wpm_value_label, "0");
    lv_obj_set_pos(wpm_value_label, 8, 66);  /* 3px down */
    LOG_INF("[INIT] WPM created");

    /* ===== 4. Connection Status (TOP_RIGHT) ===== */
    LOG_INF("[INIT] Creating connection status...");
    transport_label = lv_label_create(screen);
    lv_obj_set_style_text_font(transport_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(transport_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(transport_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_recolor(transport_label, true);
    lv_obj_align(transport_label, LV_ALIGN_TOP_RIGHT, -10, 53);

    /* Initial state: BLE with profile on new line */
    lv_label_set_text(transport_label, "#ffffff BLE#\n#ffffff 0#");

    /* Profile label kept but hidden (integrated into transport_label) */
    ble_profile_label = lv_label_create(screen);
    lv_obj_set_style_text_font(ble_profile_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_profile_label, lv_color_white(), 0);
    lv_label_set_text(ble_profile_label, "");  /* Hidden - integrated */
    lv_obj_align(ble_profile_label, LV_ALIGN_TOP_RIGHT, -8, 78);
    LOG_INF("[INIT] connection status created");

    /* ===== 5. Layer Widget (CENTER area, y=85-120) ===== */
    LOG_INF("[INIT] Creating layer widget...");
    layer_title_label = lv_label_create(screen);
    lv_obj_set_style_text_font(layer_title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(layer_title_label, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_opa(layer_title_label, LV_OPA_70, 0);
    lv_label_set_text(layer_title_label, "Layer");
    lv_obj_align(layer_title_label, LV_ALIGN_TOP_MID, 0, 82);  /* 3px up */

    int num_layers = 7;
    int spacing = 25;
    int total_width = (num_layers - 1) * spacing;
    int start_x = 140 - (total_width / 2);

    for (int i = 0; i < num_layers; i++) {
        layer_labels[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(layer_labels[i], &lv_font_montserrat_28, 0);

        char text[2] = {(char)('0' + i), '\0'};
        lv_label_set_text(layer_labels[i], text);

        if (i == active_layer) {
            lv_obj_set_style_text_color(layer_labels[i], get_layer_color(i), 0);
            lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_color(layer_labels[i], lv_color_make(40, 40, 40), 0);
            lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_30, 0);
        }

        lv_obj_set_pos(layer_labels[i], start_x + (i * spacing), 105);
    }
    LOG_INF("[INIT] layer widget created");

    /* ===== 6. Modifier Widget (CENTER, y=145) - NerdFont icons ===== */
    LOG_INF("[INIT] Creating modifier widget with NerdFont...");
    modifier_label = lv_label_create(screen);
    lv_obj_set_style_text_font(modifier_label, &NerdFonts_Regular_40, 0);
    lv_obj_set_style_text_color(modifier_label, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(modifier_label, 10, 0);  /* Space between icons */
    lv_label_set_text(modifier_label, "");  /* Empty initially */
    lv_obj_align(modifier_label, LV_ALIGN_TOP_MID, 0, 145);
    LOG_INF("[INIT] modifier widget created");

    /* ===== 7. Keyboard Battery (using BOTTOM_MID alignment like original) ===== */
    LOG_INF("[INIT] Creating keyboard battery...");

    /* Position constants (same as scanner_battery_widget.c) */
    #define KB_BAR_WIDTH       110
    #define KB_BAR_HEIGHT      4
    #define KB_BAR_Y_OFFSET    -33    /* Distance from bottom (5px lower) */
    #define KB_LABEL_Y_OFFSET  -42    /* Label above bar (3px down) */
    #define KB_LEFT_X_OFFSET   -70    /* Left battery x offset from center */
    #define KB_RIGHT_X_OFFSET  70     /* Right battery x offset from center */

    /* === Left battery === */
    /* Connected state bar - initially hidden (opa=0) */
    left_bat_bar = lv_bar_create(screen);
    lv_obj_set_size(left_bat_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(left_bat_bar, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_bar_set_range(left_bat_bar, 0, 100);
    lv_bar_set_value(left_bat_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(left_bat_bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(left_bat_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(left_bat_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(left_bat_bar, lv_color_hex(0x909090), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(left_bat_bar, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(left_bat_bar, lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(left_bat_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(left_bat_bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_opa(left_bat_bar, 0, LV_PART_MAIN);  /* Initially hidden */
    lv_obj_set_style_opa(left_bat_bar, 0, LV_PART_INDICATOR);

    /* Connected state label - initially hidden */
    left_bat_label = lv_label_create(screen);
    lv_obj_set_style_text_font(left_bat_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(left_bat_label, lv_color_white(), 0);
    lv_obj_align(left_bat_label, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(left_bat_label, "0");
    lv_obj_set_style_opa(left_bat_label, 0, 0);  /* Initially hidden */

    /* Disconnected state bar - initially visible */
    left_bat_nc_bar = lv_obj_create(screen);
    lv_obj_set_size(left_bat_nc_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(left_bat_nc_bar, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_obj_set_style_bg_color(left_bat_nc_bar, lv_color_hex(0x9e2121), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(left_bat_nc_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(left_bat_nc_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(left_bat_nc_bar, 0, 0);
    lv_obj_set_style_pad_all(left_bat_nc_bar, 0, 0);
    lv_obj_set_style_opa(left_bat_nc_bar, 255, 0);  /* Initially visible */

    /* Disconnected state label (× symbol) - initially visible */
    left_bat_nc_label = lv_label_create(screen);
    lv_obj_set_style_text_font(left_bat_nc_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(left_bat_nc_label, lv_color_hex(0xe63030), 0);
    lv_obj_align(left_bat_nc_label, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(left_bat_nc_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_opa(left_bat_nc_label, 255, 0);  /* Initially visible */

    /* === Right battery === */
    /* Connected state bar - initially hidden (opa=0) */
    right_bat_bar = lv_bar_create(screen);
    lv_obj_set_size(right_bat_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(right_bat_bar, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_bar_set_range(right_bat_bar, 0, 100);
    lv_bar_set_value(right_bat_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(right_bat_bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(right_bat_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(right_bat_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(right_bat_bar, lv_color_hex(0x909090), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(right_bat_bar, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(right_bat_bar, lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(right_bat_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(right_bat_bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_opa(right_bat_bar, 0, LV_PART_MAIN);  /* Initially hidden */
    lv_obj_set_style_opa(right_bat_bar, 0, LV_PART_INDICATOR);

    /* Connected state label - initially hidden */
    right_bat_label = lv_label_create(screen);
    lv_obj_set_style_text_font(right_bat_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(right_bat_label, lv_color_white(), 0);
    lv_obj_align(right_bat_label, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(right_bat_label, "0");
    lv_obj_set_style_opa(right_bat_label, 0, 0);  /* Initially hidden */

    /* Disconnected state bar - initially visible */
    right_bat_nc_bar = lv_obj_create(screen);
    lv_obj_set_size(right_bat_nc_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(right_bat_nc_bar, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_obj_set_style_bg_color(right_bat_nc_bar, lv_color_hex(0x9e2121), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(right_bat_nc_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(right_bat_nc_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(right_bat_nc_bar, 0, 0);
    lv_obj_set_style_pad_all(right_bat_nc_bar, 0, 0);
    lv_obj_set_style_opa(right_bat_nc_bar, 255, 0);  /* Initially visible */

    /* Disconnected state label (× symbol) - initially visible */
    right_bat_nc_label = lv_label_create(screen);
    lv_obj_set_style_text_font(right_bat_nc_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(right_bat_nc_label, lv_color_hex(0xe63030), 0);
    lv_obj_align(right_bat_nc_label, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(right_bat_nc_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_opa(right_bat_nc_label, 255, 0);  /* Initially visible */

    LOG_INF("[INIT] keyboard battery created");

    /* ===== 8. Signal Status (BOTTOM, y=220) ===== */
    LOG_INF("[INIT] Creating signal status...");

    channel_label = lv_label_create(screen);
    lv_obj_set_style_text_font(channel_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(channel_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(channel_label, "Ch:0");
    lv_obj_set_pos(channel_label, 62, 219);  /* 5px down, 5px left */

    rx_title_label = lv_label_create(screen);
    lv_obj_set_style_text_font(rx_title_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rx_title_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(rx_title_label, "RX:");
    lv_obj_set_pos(rx_title_label, 102, 219);  /* 5px down, 5px left */

    rssi_bar = lv_bar_create(screen);
    lv_obj_set_size(rssi_bar, 30, 8);
    lv_obj_set_pos(rssi_bar, 130, 221);  /* 5px down, 5px left */
    lv_bar_set_range(rssi_bar, 0, 5);
    lv_bar_set_value(rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(rssi_bar, lv_color_make(0x20, 0x20, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rssi_bar, get_rssi_color(0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(rssi_bar, 2, LV_PART_INDICATOR);

    rssi_label = lv_label_create(screen);
    lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rssi_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(rssi_label, "0dBm");
    lv_obj_set_pos(rssi_label, 167, 219);  /* 5px down, 5px left */

    rate_label = lv_label_create(screen);
    lv_obj_set_style_text_font(rate_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rate_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(rate_label, "0.0Hz");
    lv_obj_set_pos(rate_label, 222, 219);  /* 5px down, 5px left */
    LOG_INF("[INIT] signal status created");

    LOG_INF("=============================================");
    LOG_INF("=== Full Widget Test Complete ===");
    LOG_INF("=== Swipe DOWN for Settings, UP to return ===");
    LOG_INF("=============================================");

    /* Save screen reference for screen transitions */
    screen_obj = screen;
    current_screen = SCREEN_MAIN;

    return screen;
}

/* ========== Widget Update Functions (called from scanner_stub.c) ========== */

void display_update_device_name(const char *name) {
    if (device_name_label && name) {
        lv_label_set_text(device_name_label, name);
    }
}

void display_update_scanner_battery(int level) {
    scanner_battery = level;

    if (scanner_bat_icon) {
        lv_label_set_text(scanner_bat_icon, get_battery_icon(level));
        lv_obj_set_style_text_color(scanner_bat_icon, get_scanner_battery_color(level), 0);
    }

    if (scanner_bat_pct) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", level);
        lv_label_set_text(scanner_bat_pct, buf);
        lv_obj_set_style_text_color(scanner_bat_pct, get_scanner_battery_color(level), 0);
    }
}

void display_update_layer(int layer) {
    if (layer < 0 || layer > 9) return;

    /* Update active layer */
    active_layer = layer;

    int num_layers = 7;
    for (int i = 0; i < num_layers && layer_labels[i]; i++) {
        if (i == active_layer) {
            lv_obj_set_style_text_color(layer_labels[i], get_layer_color(i), 0);
            lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_color(layer_labels[i], lv_color_make(40, 40, 40), 0);
            lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_30, 0);
        }
    }
}

void display_update_wpm(int wpm) {
    if (wpm_value_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", wpm);
        lv_label_set_text(wpm_value_label, buf);
    }
}

void display_update_connection(bool usb_rdy, bool ble_conn, int profile) {
    usb_ready = usb_rdy;
    ble_connected = ble_conn;
    ble_profile = profile;

    if (transport_label) {
        /* Exclusive display: USB or BLE (not both) */
        if (usb_ready) {
            /* USB connected - show USB only */
            lv_label_set_text(transport_label, "#ffffff USB#");
        } else {
            /* USB not connected - show BLE with profile number on new line */
            const char *ble_color = ble_connected ? "00ff00" : "ffffff";
            char transport_text[32];
            snprintf(transport_text, sizeof(transport_text),
                    "#%s BLE#\n#ffffff %d#", ble_color, profile);
            lv_label_set_text(transport_label, transport_text);
        }
    }

    /* Hide profile label - now integrated into transport_label */
    if (ble_profile_label) {
        lv_label_set_text(ble_profile_label, "");
    }
}

void display_update_modifiers(uint8_t mods) {
    if (modifier_label) {
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
}

void display_update_keyboard_battery(int left, int right) {
    battery_left = left;
    battery_right = right;

    /* Left battery - toggle between connected/disconnected state */
    if (left > 0) {
        /* Connected: show bar and percentage, hide × */
        if (left_bat_nc_bar) lv_obj_set_style_opa(left_bat_nc_bar, 0, 0);
        if (left_bat_nc_label) lv_obj_set_style_opa(left_bat_nc_label, 0, 0);
        if (left_bat_bar) {
            lv_obj_set_style_opa(left_bat_bar, 255, LV_PART_MAIN);
            lv_obj_set_style_opa(left_bat_bar, 255, LV_PART_INDICATOR);
            lv_bar_set_value(left_bat_bar, left, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(left_bat_bar, get_keyboard_battery_color(left), LV_PART_INDICATOR);
        }
        if (left_bat_label) {
            lv_obj_set_style_opa(left_bat_label, 255, 0);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", left);
            lv_label_set_text(left_bat_label, buf);
            lv_obj_set_style_text_color(left_bat_label, get_keyboard_battery_color(left), 0);
        }
    } else {
        /* Disconnected: show ×, hide bar and percentage */
        if (left_bat_bar) {
            lv_obj_set_style_opa(left_bat_bar, 0, LV_PART_MAIN);
            lv_obj_set_style_opa(left_bat_bar, 0, LV_PART_INDICATOR);
        }
        if (left_bat_label) lv_obj_set_style_opa(left_bat_label, 0, 0);
        if (left_bat_nc_bar) lv_obj_set_style_opa(left_bat_nc_bar, 255, 0);
        if (left_bat_nc_label) lv_obj_set_style_opa(left_bat_nc_label, 255, 0);
    }

    /* Right battery - toggle between connected/disconnected state */
    if (right > 0) {
        /* Connected: show bar and percentage, hide × */
        if (right_bat_nc_bar) lv_obj_set_style_opa(right_bat_nc_bar, 0, 0);
        if (right_bat_nc_label) lv_obj_set_style_opa(right_bat_nc_label, 0, 0);
        if (right_bat_bar) {
            lv_obj_set_style_opa(right_bat_bar, 255, LV_PART_MAIN);
            lv_obj_set_style_opa(right_bat_bar, 255, LV_PART_INDICATOR);
            lv_bar_set_value(right_bat_bar, right, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(right_bat_bar, get_keyboard_battery_color(right), LV_PART_INDICATOR);
        }
        if (right_bat_label) {
            lv_obj_set_style_opa(right_bat_label, 255, 0);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", right);
            lv_label_set_text(right_bat_label, buf);
            lv_obj_set_style_text_color(right_bat_label, get_keyboard_battery_color(right), 0);
        }
    } else {
        /* Disconnected: show ×, hide bar and percentage */
        if (right_bat_bar) {
            lv_obj_set_style_opa(right_bat_bar, 0, LV_PART_MAIN);
            lv_obj_set_style_opa(right_bat_bar, 0, LV_PART_INDICATOR);
        }
        if (right_bat_label) lv_obj_set_style_opa(right_bat_label, 0, 0);
        if (right_bat_nc_bar) lv_obj_set_style_opa(right_bat_nc_bar, 255, 0);
        if (right_bat_nc_label) lv_obj_set_style_opa(right_bat_nc_label, 255, 0);
    }
}

void display_update_signal(int8_t rssi_val, float rate) {
    rssi = rssi_val;
    rate_hz = rate;

    uint8_t bars = rssi_to_bars(rssi_val);

    if (rssi_bar) {
        lv_bar_set_value(rssi_bar, bars, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(rssi_bar, get_rssi_color(bars), LV_PART_INDICATOR);
    }

    if (rssi_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ddBm", rssi_val);
        lv_label_set_text(rssi_label, buf);
    }

    if (rate_label) {
        char buf[16];
        int rate_int = (int)(rate * 10);
        snprintf(buf, sizeof(buf), "%d.%dHz", rate_int / 10, rate_int % 10);
        lv_label_set_text(rate_label, buf);
    }
}

/* ========== Screen Transition Functions ========== */

static void destroy_main_screen_widgets(void) {
    LOG_INF("Destroying main screen widgets...");

    if (rate_label) { lv_obj_del(rate_label); rate_label = NULL; }
    if (rssi_label) { lv_obj_del(rssi_label); rssi_label = NULL; }
    if (rssi_bar) { lv_obj_del(rssi_bar); rssi_bar = NULL; }
    if (rx_title_label) { lv_obj_del(rx_title_label); rx_title_label = NULL; }
    if (channel_label) { lv_obj_del(channel_label); channel_label = NULL; }
    /* Keyboard battery - nc elements */
    if (right_bat_nc_label) { lv_obj_del(right_bat_nc_label); right_bat_nc_label = NULL; }
    if (right_bat_nc_bar) { lv_obj_del(right_bat_nc_bar); right_bat_nc_bar = NULL; }
    if (left_bat_nc_label) { lv_obj_del(left_bat_nc_label); left_bat_nc_label = NULL; }
    if (left_bat_nc_bar) { lv_obj_del(left_bat_nc_bar); left_bat_nc_bar = NULL; }
    /* Keyboard battery - connected elements */
    if (right_bat_bar) { lv_obj_del(right_bat_bar); right_bat_bar = NULL; }
    if (right_bat_label) { lv_obj_del(right_bat_label); right_bat_label = NULL; }
    if (left_bat_bar) { lv_obj_del(left_bat_bar); left_bat_bar = NULL; }
    if (left_bat_label) { lv_obj_del(left_bat_label); left_bat_label = NULL; }
    if (modifier_label) { lv_obj_del(modifier_label); modifier_label = NULL; }
    for (int i = 0; i < 10; i++) {
        if (layer_labels[i]) { lv_obj_del(layer_labels[i]); layer_labels[i] = NULL; }
    }
    if (layer_title_label) { lv_obj_del(layer_title_label); layer_title_label = NULL; }
    if (ble_profile_label) { lv_obj_del(ble_profile_label); ble_profile_label = NULL; }
    if (transport_label) { lv_obj_del(transport_label); transport_label = NULL; }
    if (wpm_value_label) { lv_obj_del(wpm_value_label); wpm_value_label = NULL; }
    if (wpm_title_label) { lv_obj_del(wpm_title_label); wpm_title_label = NULL; }
    if (scanner_bat_pct) { lv_obj_del(scanner_bat_pct); scanner_bat_pct = NULL; }
    if (scanner_bat_icon) { lv_obj_del(scanner_bat_icon); scanner_bat_icon = NULL; }
    if (device_name_label) { lv_obj_del(device_name_label); device_name_label = NULL; }

    LOG_INF("Main screen widgets destroyed");
}

static void create_main_screen_widgets(void) {
    if (!screen_obj) return;
    LOG_INF("Creating main screen widgets...");

    /* Recreate all main screen widgets using screen_obj */
    device_name_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(device_name_label, lv_color_white(), 0);
    lv_label_set_text(device_name_label, "Scanning...");
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 25);

    scanner_bat_icon = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(scanner_bat_icon, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(scanner_bat_icon, 216, 4);  /* 4px right */
    lv_label_set_text(scanner_bat_icon, LV_SYMBOL_BATTERY_3);  /* Initial: 3/4 battery */
    lv_obj_set_style_text_color(scanner_bat_icon, lv_color_hex(0x7FFF00), 0);

    scanner_bat_pct = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(scanner_bat_pct, &lv_font_unscii_8, 0);
    lv_obj_set_pos(scanner_bat_pct, 238, 7);  /* 2px up */
    lv_label_set_text(scanner_bat_pct, "?");  /* Unknown until battery read */
    lv_obj_set_style_text_color(scanner_bat_pct, lv_color_hex(0x7FFF00), 0);

    wpm_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(wpm_title_label, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(wpm_title_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(wpm_title_label, "WPM");
    lv_obj_set_pos(wpm_title_label, 20, 53);  /* 3px down */

    wpm_value_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(wpm_value_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(wpm_value_label, lv_color_white(), 0);
    lv_obj_set_width(wpm_value_label, 48);  /* Fixed width for centering */
    lv_obj_set_style_text_align(wpm_value_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(wpm_value_label, "0");
    lv_obj_set_pos(wpm_value_label, 8, 66);  /* 3px down */

    transport_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(transport_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(transport_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(transport_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_recolor(transport_label, true);
    lv_obj_align(transport_label, LV_ALIGN_TOP_RIGHT, -10, 53);
    lv_label_set_text(transport_label, "#ffffff BLE#\n#ffffff 0#");  /* Exclusive display */

    ble_profile_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ble_profile_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ble_profile_label, lv_color_white(), 0);
    lv_label_set_text(ble_profile_label, "");  /* Hidden - integrated */
    lv_obj_align(ble_profile_label, LV_ALIGN_TOP_RIGHT, -8, 78);

    layer_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(layer_title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(layer_title_label, lv_color_make(160, 160, 160), 0);
    lv_label_set_text(layer_title_label, "Layer");
    lv_obj_align(layer_title_label, LV_ALIGN_TOP_MID, 0, 82);  /* 3px up */

    int num_layers = 7, spacing = 25;
    int start_x = 140 - ((num_layers - 1) * spacing / 2);
    for (int i = 0; i < num_layers; i++) {
        layer_labels[i] = lv_label_create(screen_obj);
        lv_obj_set_style_text_font(layer_labels[i], &lv_font_montserrat_28, 0);
        char text[2] = {(char)('0' + i), '\0'};
        lv_label_set_text(layer_labels[i], text);
        lv_obj_set_style_text_color(layer_labels[i],
            (i == active_layer) ? get_layer_color(i) : lv_color_make(40, 40, 40), 0);
        lv_obj_set_style_text_opa(layer_labels[i],
            (i == active_layer) ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_pos(layer_labels[i], start_x + (i * spacing), 105);
    }

    modifier_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(modifier_label, &NerdFonts_Regular_40, 0);
    lv_obj_set_style_text_color(modifier_label, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(modifier_label, 10, 0);  /* Space between icons */
    lv_label_set_text(modifier_label, "");
    lv_obj_align(modifier_label, LV_ALIGN_TOP_MID, 0, 145);

    /* === Left battery (using BOTTOM_MID alignment) === */
    /* Connected state bar - initially hidden */
    left_bat_bar = lv_bar_create(screen_obj);
    lv_obj_set_size(left_bat_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(left_bat_bar, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_bar_set_range(left_bat_bar, 0, 100);
    lv_bar_set_value(left_bat_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(left_bat_bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(left_bat_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(left_bat_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(left_bat_bar, lv_color_hex(0x909090), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(left_bat_bar, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(left_bat_bar, lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(left_bat_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(left_bat_bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_opa(left_bat_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(left_bat_bar, 0, LV_PART_INDICATOR);

    /* Connected state label - initially hidden */
    left_bat_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(left_bat_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(left_bat_label, lv_color_white(), 0);
    lv_obj_align(left_bat_label, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(left_bat_label, "0");
    lv_obj_set_style_opa(left_bat_label, 0, 0);

    /* Disconnected state bar - initially visible */
    left_bat_nc_bar = lv_obj_create(screen_obj);
    lv_obj_set_size(left_bat_nc_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(left_bat_nc_bar, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_obj_set_style_bg_color(left_bat_nc_bar, lv_color_hex(0x9e2121), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(left_bat_nc_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(left_bat_nc_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(left_bat_nc_bar, 0, 0);
    lv_obj_set_style_pad_all(left_bat_nc_bar, 0, 0);
    lv_obj_set_style_opa(left_bat_nc_bar, 255, 0);

    /* Disconnected state label (× symbol) - initially visible */
    left_bat_nc_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(left_bat_nc_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(left_bat_nc_label, lv_color_hex(0xe63030), 0);
    lv_obj_align(left_bat_nc_label, LV_ALIGN_BOTTOM_MID, KB_LEFT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(left_bat_nc_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_opa(left_bat_nc_label, 255, 0);

    /* === Right battery (using BOTTOM_MID alignment) === */
    /* Connected state bar - initially hidden */
    right_bat_bar = lv_bar_create(screen_obj);
    lv_obj_set_size(right_bat_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(right_bat_bar, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_bar_set_range(right_bat_bar, 0, 100);
    lv_bar_set_value(right_bat_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(right_bat_bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(right_bat_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(right_bat_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(right_bat_bar, lv_color_hex(0x909090), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(right_bat_bar, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(right_bat_bar, lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(right_bat_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(right_bat_bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_opa(right_bat_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(right_bat_bar, 0, LV_PART_INDICATOR);

    /* Connected state label - initially hidden */
    right_bat_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(right_bat_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(right_bat_label, lv_color_white(), 0);
    lv_obj_align(right_bat_label, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(right_bat_label, "0");
    lv_obj_set_style_opa(right_bat_label, 0, 0);

    /* Disconnected state bar - initially visible */
    right_bat_nc_bar = lv_obj_create(screen_obj);
    lv_obj_set_size(right_bat_nc_bar, KB_BAR_WIDTH, KB_BAR_HEIGHT);
    lv_obj_align(right_bat_nc_bar, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_BAR_Y_OFFSET);
    lv_obj_set_style_bg_color(right_bat_nc_bar, lv_color_hex(0x9e2121), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(right_bat_nc_bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(right_bat_nc_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(right_bat_nc_bar, 0, 0);
    lv_obj_set_style_pad_all(right_bat_nc_bar, 0, 0);
    lv_obj_set_style_opa(right_bat_nc_bar, 255, 0);

    /* Disconnected state label (× symbol) - initially visible */
    right_bat_nc_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(right_bat_nc_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(right_bat_nc_label, lv_color_hex(0xe63030), 0);
    lv_obj_align(right_bat_nc_label, LV_ALIGN_BOTTOM_MID, KB_RIGHT_X_OFFSET, KB_LABEL_Y_OFFSET);
    lv_label_set_text(right_bat_nc_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_opa(right_bat_nc_label, 255, 0);

    channel_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(channel_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(channel_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(channel_label, "Ch:0");
    lv_obj_set_pos(channel_label, 62, 219);  /* 5px down, 5px left */

    rx_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(rx_title_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rx_title_label, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(rx_title_label, "RX:");
    lv_obj_set_pos(rx_title_label, 102, 219);  /* 5px down, 5px left */

    rssi_bar = lv_bar_create(screen_obj);
    lv_obj_set_size(rssi_bar, 30, 8);
    lv_obj_set_pos(rssi_bar, 130, 221);  /* 5px down, 5px left */
    lv_bar_set_range(rssi_bar, 0, 5);
    lv_bar_set_value(rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(rssi_bar, lv_color_hex(0x202020), LV_PART_MAIN);

    rssi_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rssi_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(rssi_label, "--dBm");
    lv_obj_set_pos(rssi_label, 167, 219);  /* 5px down, 5px left */

    rate_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(rate_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rate_label, lv_color_make(0xA0, 0xA0, 0xA0), 0);
    lv_label_set_text(rate_label, "0.0Hz");
    lv_obj_set_pos(rate_label, 222, 219);  /* 5px down, 5px left */

    LOG_INF("Main screen widgets created");
}

static void destroy_settings_screen_widgets(void) {
    LOG_INF("Destroying settings screen widgets...");
    if (settings_back_hint) { lv_obj_del(settings_back_hint); settings_back_hint = NULL; }
    if (settings_info_label) { lv_obj_del(settings_info_label); settings_info_label = NULL; }
    if (settings_title_label) { lv_obj_del(settings_title_label); settings_title_label = NULL; }
    LOG_INF("Settings screen widgets destroyed");
}

static void create_settings_screen_widgets(void) {
    if (!screen_obj) return;
    LOG_INF("Creating settings screen widgets...");

    /* Simple settings screen for testing */
    settings_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(settings_title_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(settings_title_label, lv_color_hex(0x00BFFF), 0);
    lv_label_set_text(settings_title_label, "Settings");
    lv_obj_align(settings_title_label, LV_ALIGN_TOP_MID, 0, 40);

    settings_info_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(settings_info_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(settings_info_label, lv_color_white(), 0);
    lv_label_set_text(settings_info_label, "Screen transition test\n\nSwipe UP to go back");
    lv_obj_set_style_text_align(settings_info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(settings_info_label, LV_ALIGN_CENTER, 0, 0);

    settings_back_hint = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(settings_back_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(settings_back_hint, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(settings_back_hint, LV_SYMBOL_UP " Swipe UP");
    lv_obj_align(settings_back_hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    LOG_INF("Settings screen widgets created");
}

/* ========== Swipe Event Handler ========== */

static int swipe_gesture_listener(const zmk_event_t *eh) {
    const struct zmk_swipe_gesture_event *ev = as_zmk_swipe_gesture_event(eh);
    if (ev == NULL) return ZMK_EV_EVENT_BUBBLE;

    LOG_INF("Swipe event received: direction=%d, current_screen=%d",
            ev->direction, current_screen);

    /* Set transition flag to protect work queue updates */
    transition_in_progress = true;

    switch (ev->direction) {
    case SWIPE_DIRECTION_DOWN:
        if (current_screen == SCREEN_MAIN) {
            LOG_INF(">>> Transitioning: MAIN -> SETTINGS");
            lv_obj_set_style_bg_color(screen_obj, lv_color_hex(0x1a1a2e), 0);
            destroy_main_screen_widgets();
            create_settings_screen_widgets();
            current_screen = SCREEN_SETTINGS;
            LOG_INF(">>> Transition complete");
        }
        break;

    case SWIPE_DIRECTION_UP:
        if (current_screen == SCREEN_SETTINGS) {
            LOG_INF(">>> Transitioning: SETTINGS -> MAIN");
            lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
            destroy_settings_screen_widgets();
            create_main_screen_widgets();
            current_screen = SCREEN_MAIN;
            LOG_INF(">>> Transition complete");
        }
        break;

    default:
        LOG_INF("Swipe direction not handled: %d", ev->direction);
        break;
    }

    /* Clear transition flag */
    transition_in_progress = false;

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(swipe_gesture, swipe_gesture_listener);
ZMK_SUBSCRIPTION(swipe_gesture, zmk_swipe_gesture_event);
