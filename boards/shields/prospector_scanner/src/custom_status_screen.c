/**
 * Prospector Scanner UI - Full Widget Test
 * NO CONTAINER PATTERN - All widgets use absolute positioning
 *
 * Screen: 280x240 (90 degree rotated from 240x280)
 *
 * Supports screen transitions via swipe gestures:
 * - Main Screen → DOWN → Display Settings
 * - Main Screen → RIGHT → Quick Actions (System Settings)
 * - Display Settings → UP → Main Screen
 * - Quick Actions → LEFT → Main Screen
 *
 * CRITICAL DESIGN PRINCIPLES (from CLAUDE.md):
 * 1. ISR/Callback から LVGL API を呼ばない - フラグを立てるだけ
 * 2. すべての処理は Main Task (LVGL timer) で実行
 * 3. コンテナを使用しない - すべて絶対座標で配置
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/retention/bootmode.h>  /* For bootmode_set() - Zephyr 4.x bootloader entry */
#include <zephyr/drivers/led.h>  /* For PWM backlight control */
#include <string.h>
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/status_scanner.h>
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif
#include "events/swipe_gesture_event.h"
#include "fonts.h"  /* NerdFont declarations */
#include "touch_handler.h"  /* For LVGL input device registration */
#include "brightness_control.h"  /* For auto brightness sensor control */

LOG_MODULE_REGISTER(display_screen, LOG_LEVEL_INF);

/* ========== Screen State Management ========== */
enum screen_state {
    SCREEN_MAIN = 0,
    SCREEN_DISPLAY_SETTINGS,
    SCREEN_SYSTEM_SETTINGS,
    SCREEN_KEYBOARD_SELECT,
};

static enum screen_state current_screen = SCREEN_MAIN;
static lv_obj_t *screen_obj = NULL;

/* Transition protection flag - checked by work queues */
volatile bool transition_in_progress = false;

/* Pending swipe direction - set by ISR listener, processed by LVGL timer */
static volatile enum swipe_direction pending_swipe = SWIPE_DIRECTION_NONE;
static lv_timer_t *swipe_process_timer = NULL;

/* Auto brightness timer - reads sensor and adjusts brightness when auto mode enabled */
static lv_timer_t *auto_brightness_timer = NULL;
#define AUTO_BRIGHTNESS_INTERVAL_MS 1000  /* Check sensor every 1 second */

/* Forward declarations */
static void destroy_main_screen_widgets(void);
static void create_main_screen_widgets(void);
static void destroy_display_settings_widgets(void);
static void create_display_settings_widgets(void);
static void destroy_system_settings_widgets(void);
static void create_system_settings_widgets(void);
static void destroy_keyboard_select_widgets(void);
static void create_keyboard_select_widgets(void);
static void swipe_process_timer_cb(lv_timer_t *timer);

/* Custom slider state for inverted drag handling */
/* Due to 180° touch panel rotation, LVGL X decreases when user drags right */
/* We track touch position manually and invert the calculation */
static struct {
    lv_obj_t *active_slider;
    int32_t start_x;       /* Touch X at drag start */
    int32_t start_y;       /* Touch Y at drag start (for swipe detection) */
    int32_t start_value;   /* Slider value at drag start */
    int32_t current_value; /* Current calculated value (saved for RELEASED) */
    int32_t min_val;
    int32_t max_val;
    int32_t slider_width;  /* Slider track width in pixels */
    bool drag_cancelled;   /* True if vertical swipe detected - let swipe through */
} slider_drag_state = {0};

/* Threshold for detecting vertical swipe vs horizontal drag */
#define SLIDER_SWIPE_THRESHOLD 30

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

/* ========== Cached data (updated by scanner, preserved across screen transitions) ========== */
static int active_layer = 0;
static int wpm_value = 0;
static int battery_left = 0;
static int battery_right = 0;
static int scanner_battery = 0;
static int rssi = -100;  /* Default: very weak signal */
static float rate_hz = 0.0f;
static int ble_profile = 0;
static bool usb_ready = false;
static bool ble_connected = false;
static bool ble_bonded = false;
static char cached_device_name[32] = "Scanning...";
static uint8_t cached_modifiers = 0;

/* ========== PWM Backlight Control ========== */
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
#define BACKLIGHT_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(pwm_leds)
static const struct device *backlight_dev = DEVICE_DT_GET(BACKLIGHT_NODE);
#else
static const struct device *backlight_dev = NULL;
#endif

static void set_pwm_brightness(uint8_t brightness) {
    if (!backlight_dev || !device_is_ready(backlight_dev)) {
        LOG_WRN("Backlight device not ready");
        return;
    }
    /* Ensure minimum brightness of 1% to prevent screen from going completely dark */
    if (brightness < 1) {
        brightness = 1;
    }
    /* INVERT: Backlight circuit is inverted (100% PWM = dark, 0% = bright)
     * So we invert: user's 100% brightness → 0% PWM duty, 1% brightness → 99% PWM */
    uint8_t pwm_value = 100 - brightness;
    int ret = led_set_brightness(backlight_dev, 0, pwm_value);
    if (ret < 0) {
        LOG_ERR("Failed to set brightness: %d", ret);
    } else {
        LOG_INF("Backlight: user=%d%% -> PWM=%d%%", brightness, pwm_value);
    }
}

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

/* ========== Display Settings Screen Widgets (NO CONTAINER) ========== */
static lv_obj_t *ds_title_label = NULL;
static lv_obj_t *ds_brightness_label = NULL;
static lv_obj_t *ds_auto_label = NULL;
static lv_obj_t *ds_auto_switch = NULL;
static lv_obj_t *ds_brightness_slider = NULL;
static lv_obj_t *ds_brightness_value = NULL;
static lv_obj_t *ds_battery_label = NULL;
static lv_obj_t *ds_battery_switch = NULL;
static lv_obj_t *ds_layer_label = NULL;
static lv_obj_t *ds_layer_slider = NULL;
static lv_obj_t *ds_layer_value = NULL;
static lv_obj_t *ds_nav_hint = NULL;

/* Display Settings State (persists across screen transitions) */
static bool ds_auto_brightness_enabled = false;
static uint8_t ds_manual_brightness = 65;
static bool ds_battery_visible = true;
static uint8_t ds_max_layers = 7;

/* UI interaction flag - prevents swipe during slider drag */
static bool ui_interaction_active = false;

/* LVGL input device registration flag - register once when first settings screen shown */
static bool lvgl_indev_registered = false;

/* ========== System Settings Screen Widgets (NO CONTAINER) ========== */
static lv_obj_t *ss_title_label = NULL;
static lv_obj_t *ss_bootloader_btn = NULL;
static lv_obj_t *ss_reset_btn = NULL;
static lv_obj_t *ss_nav_hint = NULL;

/* ========== Keyboard Select Screen Widgets (NO CONTAINER) ========== */
#define KS_MAX_KEYBOARDS 6  /* Maximum displayable keyboards */
static lv_obj_t *ks_title_label = NULL;
static lv_obj_t *ks_nav_hint = NULL;
static lv_timer_t *ks_update_timer = NULL;
static int ks_selected_keyboard = -1;  /* Currently selected keyboard index */

/* Per-keyboard entry widgets */
struct ks_keyboard_entry {
    lv_obj_t *container;     /* Clickable container */
    lv_obj_t *name_label;    /* Keyboard name */
    lv_obj_t *rssi_bar;      /* Signal strength bar */
    lv_obj_t *rssi_label;    /* RSSI dBm value */
    int keyboard_index;      /* Index in scanner's keyboard array */
};
static struct ks_keyboard_entry ks_entries[KS_MAX_KEYBOARDS] = {0};
static uint8_t ks_entry_count = 0;

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
    int label_width = 20;  /* Fixed width for center alignment */
    int total_width = (num_layers - 1) * spacing;
    int start_x = 140 - (total_width / 2) - (label_width / 2);  /* Offset by half label width */

    for (int i = 0; i < num_layers; i++) {
        layer_labels[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(layer_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_set_width(layer_labels[i], label_width);
        lv_obj_set_style_text_align(layer_labels[i], LV_TEXT_ALIGN_CENTER, 0);

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
    lv_obj_set_pos(rssi_bar, 130, 223);  /* RX indicator position */
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

    /* Register LVGL timer for swipe processing in main thread
     * This timer checks the pending_swipe flag every 50ms and processes
     * screen transitions safely in the LVGL timer context (main thread).
     *
     * DESIGN: ISR sets flag → LVGL timer processes → Thread-safe LVGL ops
     */
    if (!swipe_process_timer) {
        swipe_process_timer = lv_timer_create(swipe_process_timer_cb, 50, NULL);
        LOG_INF("Swipe processing timer registered (50ms interval)");
    }

    return screen;
}

/* ========== Widget Update Functions (called from scanner_stub.c) ========== */

void display_update_device_name(const char *name) {
    if (name) {
        strncpy(cached_device_name, name, sizeof(cached_device_name) - 1);
        cached_device_name[sizeof(cached_device_name) - 1] = '\0';
    }
    if (device_name_label && name) {
        lv_label_set_text(device_name_label, name);
    }
}

void display_update_scanner_battery(int level) {
    scanner_battery = level;

    /* If scanner battery widget is disabled via settings, hide it */
    if (!ds_battery_visible) {
        if (scanner_bat_icon) lv_obj_set_style_opa(scanner_bat_icon, 0, 0);
        if (scanner_bat_pct) lv_obj_set_style_opa(scanner_bat_pct, 0, 0);
        return;
    }

    /* Check if USB is connected (= charging) */
    bool is_charging = false;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    is_charging = zmk_usb_is_powered();
#endif

    /* Charging: Blue color (0x007FFF), show charge symbol + battery icon */
    lv_color_t display_color = is_charging ? lv_color_hex(0x007FFF) : get_scanner_battery_color(level);

    if (scanner_bat_icon) {
        lv_obj_set_style_opa(scanner_bat_icon, 255, 0);  /* Ensure visible */
        if (is_charging) {
            /* Show charge symbol + battery icon, move 3px left to accommodate wider icon */
            static char combined_icon[16];
            snprintf(combined_icon, sizeof(combined_icon), LV_SYMBOL_CHARGE "%s", get_battery_icon(level));
            lv_label_set_text(scanner_bat_icon, combined_icon);
            lv_obj_set_pos(scanner_bat_icon, 213, 4);  /* 3px left when charging */
        } else {
            lv_label_set_text(scanner_bat_icon, get_battery_icon(level));
            lv_obj_set_pos(scanner_bat_icon, 216, 4);  /* Normal position */
        }
        lv_obj_set_style_text_color(scanner_bat_icon, display_color, 0);
    }

    if (scanner_bat_pct) {
        lv_obj_set_style_opa(scanner_bat_pct, 255, 0);  /* Ensure visible */
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", level);
        lv_label_set_text(scanner_bat_pct, buf);
        lv_obj_set_style_text_color(scanner_bat_pct, display_color, 0);
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
    wpm_value = wpm;  /* Cache for screen transitions */
    if (wpm_value_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", wpm);
        lv_label_set_text(wpm_value_label, buf);
    }
}

void display_update_connection(bool usb_rdy, bool ble_conn, bool ble_bond, int profile) {
    usb_ready = usb_rdy;
    ble_connected = ble_conn;
    ble_bonded = ble_bond;
    ble_profile = profile;

    if (transport_label) {
        /* Exclusive display: USB or BLE (not both) */
        if (usb_ready) {
            /* USB connected - show USB only */
            lv_label_set_text(transport_label, "#ffffff USB#");
        } else {
            /* USB not connected - show BLE with profile number on new line
             * BLE text colors:
             * - Green (00ff00): Connected
             * - Blue (4A90E2): Bonded but not connected (registered profile)
             * - White (ffffff): Not bonded (empty profile)
             * Profile number: Always white
             */
            const char *ble_color;
            if (ble_conn) {
                ble_color = "00ff00";  /* Green - connected */
            } else if (ble_bond) {
                ble_color = "4A90E2";  /* Blue - bonded but not connected */
            } else {
                ble_color = "ffffff";  /* White - not bonded */
            }
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
    cached_modifiers = mods;  /* Cache for screen transitions */
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
            char buf[16];
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
            char buf[16];
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

    int num_layers = ds_max_layers, spacing = 25;  /* Use setting from Display Settings */
    int label_width = 20;  /* Fixed width for center alignment */
    int start_x = 140 - ((num_layers - 1) * spacing / 2) - (label_width / 2);
    for (int i = 0; i < num_layers; i++) {
        layer_labels[i] = lv_label_create(screen_obj);
        lv_obj_set_style_text_font(layer_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_set_width(layer_labels[i], label_width);
        lv_obj_set_style_text_align(layer_labels[i], LV_TEXT_ALIGN_CENTER, 0);
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
    lv_obj_set_pos(rssi_bar, 130, 223);  /* RX indicator position */
    lv_bar_set_range(rssi_bar, 0, 5);
    lv_bar_set_value(rssi_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(rssi_bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rssi_bar, get_rssi_color(0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(rssi_bar, 2, LV_PART_INDICATOR);

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

    LOG_INF("Main screen widgets created, restoring cached values...");

    /* Restore all cached values to newly created widgets */
    display_update_device_name(cached_device_name);
    display_update_scanner_battery(scanner_battery);
    display_update_wpm(wpm_value);
    display_update_connection(usb_ready, ble_connected, ble_bonded, ble_profile);
    display_update_layer(active_layer);
    display_update_modifiers(cached_modifiers);
    display_update_keyboard_battery(battery_left, battery_right);
    display_update_signal(rssi, rate_hz);

    LOG_INF("Cached values restored");
}

/* ========== Display Settings Event Handlers ========== */

/**
 * Custom slider drag handler - inverts drag direction and detects swipes
 *
 * Due to 180° touch panel rotation, LVGL X decreases when user drags right.
 * This handler tracks the raw touch position and manually calculates the
 * slider value with inverted direction.
 *
 * Also detects vertical swipes and cancels slider drag to allow screen navigation.
 *
 * Events handled:
 * - LV_EVENT_PRESSED: Record initial state
 * - LV_EVENT_PRESSING: Update slider value or detect vertical swipe
 * - LV_EVENT_RELEASED: Clear state, restore value if cancelled
 */
static void ds_custom_slider_drag_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    if (code == LV_EVENT_PRESSED) {
        /* Record initial state */
        slider_drag_state.active_slider = slider;
        slider_drag_state.start_x = point.x;
        slider_drag_state.start_y = point.y;
        slider_drag_state.start_value = lv_slider_get_value(slider);
        slider_drag_state.current_value = slider_drag_state.start_value;
        slider_drag_state.min_val = lv_slider_get_min_value(slider);
        slider_drag_state.max_val = lv_slider_get_max_value(slider);
        slider_drag_state.slider_width = lv_obj_get_width(slider);
        slider_drag_state.drag_cancelled = false;
        ui_interaction_active = true;
        LOG_DBG("Slider drag start: x=%d, y=%d, value=%d",
                (int)point.x, (int)point.y, (int)slider_drag_state.start_value);

    } else if (code == LV_EVENT_PRESSING) {
        if (slider_drag_state.active_slider != slider) return;
        if (slider_drag_state.drag_cancelled) return;  /* Already cancelled */

        /* Calculate deltas */
        int32_t delta_x = point.x - slider_drag_state.start_x;
        int32_t delta_y = point.y - slider_drag_state.start_y;
        int32_t abs_delta_x = (delta_x < 0) ? -delta_x : delta_x;
        int32_t abs_delta_y = (delta_y < 0) ? -delta_y : delta_y;

        /* Check for vertical swipe - if Y movement > threshold and > X movement */
        if (abs_delta_y > SLIDER_SWIPE_THRESHOLD && abs_delta_y > abs_delta_x * 2) {
            /* Cancel slider drag - restore original value */
            LOG_INF("Vertical swipe detected on slider - cancelling drag");
            lv_slider_set_value(slider, slider_drag_state.start_value, LV_ANIM_OFF);
            slider_drag_state.current_value = slider_drag_state.start_value;
            slider_drag_state.drag_cancelled = true;
            ui_interaction_active = false;  /* Allow swipe to be processed */
            return;
        }

        /* Horizontal drag - update slider value */
        /* Direct mapping - coordinate transform no longer inverts X axis */
        int32_t drag_delta = delta_x;

        /* Convert pixel delta to value delta */
        int32_t value_range = slider_drag_state.max_val - slider_drag_state.min_val;
        int32_t value_delta = (drag_delta * value_range) / slider_drag_state.slider_width;

        /* Calculate new value */
        int32_t new_value = slider_drag_state.start_value + value_delta;

        /* Clamp to range */
        if (new_value < slider_drag_state.min_val) new_value = slider_drag_state.min_val;
        if (new_value > slider_drag_state.max_val) new_value = slider_drag_state.max_val;

        /* Save the calculated value for RELEASED handler */
        slider_drag_state.current_value = new_value;

        /* Set slider value - this overrides LVGL's default handling */
        lv_slider_set_value(slider, new_value, LV_ANIM_OFF);

    } else if (code == LV_EVENT_RELEASED) {
        if (slider_drag_state.active_slider == slider) {
            /* CRITICAL: Restore our calculated value because LVGL's default handler
             * already ran and set the wrong value based on inverted coordinates */
            lv_slider_set_value(slider, slider_drag_state.current_value, LV_ANIM_OFF);

            /* Clear active_slider BEFORE sending VALUE_CHANGED so the callback knows
             * this is our final event and not LVGL's spurious one */
            bool was_cancelled = slider_drag_state.drag_cancelled;
            slider_drag_state.active_slider = NULL;
            slider_drag_state.drag_cancelled = false;
            ui_interaction_active = false;

            if (!was_cancelled) {
                /* Trigger final value changed event with correct value */
                lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, NULL);
                LOG_INF("Slider drag end: final_value=%d", (int)slider_drag_state.current_value);
            } else {
                LOG_DBG("Slider drag cancelled (swipe)");
            }
            return;
        }
        slider_drag_state.active_slider = NULL;
        slider_drag_state.drag_cancelled = false;
        ui_interaction_active = false;
    }
}

/* Auto brightness timer callback - reads sensor and updates brightness */
static void auto_brightness_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    if (!ds_auto_brightness_enabled || !brightness_control_sensor_available()) {
        return;
    }

    uint16_t light_val = 0;
    int ret = brightness_control_read_sensor(&light_val);
    if (ret != 0) {
        LOG_DBG("Auto brightness: sensor read failed (%d)", ret);
        return;
    }

    /* Map light value to brightness percentage */
    uint8_t target_brightness = brightness_control_map_light_to_brightness(light_val);

    /* Apply brightness (PWM) */
    set_pwm_brightness(target_brightness);

    LOG_DBG("Auto brightness: light=%u -> brightness=%u%%", light_val, target_brightness);
}

/* Auto brightness switch handler */
static void ds_auto_switch_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ds_auto_brightness_enabled = checked;

    /* Enable/disable the brightness control module's auto mode */
    brightness_control_set_auto(checked);

    /* Start/stop auto brightness timer */
    if (checked && brightness_control_sensor_available()) {
        if (!auto_brightness_timer) {
            auto_brightness_timer = lv_timer_create(auto_brightness_timer_cb, AUTO_BRIGHTNESS_INTERVAL_MS, NULL);
            LOG_INF("Auto brightness timer started (%d ms interval)", AUTO_BRIGHTNESS_INTERVAL_MS);
        }
        /* Trigger immediate sensor read */
        auto_brightness_timer_cb(NULL);
    } else if (auto_brightness_timer) {
        lv_timer_del(auto_brightness_timer);
        auto_brightness_timer = NULL;
        LOG_INF("Auto brightness timer stopped");
    }

    /* Enable/disable manual slider based on auto state */
    if (ds_brightness_slider) {
        if (checked) {
            lv_obj_add_state(ds_brightness_slider, LV_STATE_DISABLED);
            lv_obj_set_style_opa(ds_brightness_slider, LV_OPA_50, 0);
        } else {
            lv_obj_clear_state(ds_brightness_slider, LV_STATE_DISABLED);
            lv_obj_set_style_opa(ds_brightness_slider, LV_OPA_COVER, 0);
            /* When disabling auto, apply manual brightness setting */
            set_pwm_brightness((uint8_t)ds_manual_brightness);
        }
    }
    LOG_INF("Auto brightness: %s (sensor: %s)", checked ? "ON" : "OFF",
            brightness_control_sensor_available() ? "available" : "unavailable");
}

/* Brightness slider handler - value is already correct from custom drag handler */
static void ds_brightness_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);

    /* Ignore VALUE_CHANGED from LVGL's default handler during custom drag.
     * Our RELEASED handler clears active_slider before sending the final event. */
    if (slider_drag_state.active_slider == slider) {
        LOG_DBG("Ignoring spurious VALUE_CHANGED during drag");
        return;
    }

    int value = lv_slider_get_value(slider);
    ds_manual_brightness = (uint8_t)value;

    /* Update value label */
    if (ds_brightness_value) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(ds_brightness_value, buf);
    }

    /* Apply brightness to hardware (only if not in auto mode) */
    if (!ds_auto_brightness_enabled) {
        set_pwm_brightness((uint8_t)value);
    }
    LOG_INF("Brightness changed to %d%%", value);
}

/* Scanner battery switch handler */
static void ds_battery_switch_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    ds_battery_visible = lv_obj_has_state(sw, LV_STATE_CHECKED);
    LOG_INF("Scanner battery widget: %s", ds_battery_visible ? "visible" : "hidden");

    /* Immediately update scanner battery widget visibility using cached value */
    display_update_scanner_battery(scanner_battery);
}

/* Layer slider handler - value is already correct from custom drag handler */
static void ds_layer_slider_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *slider = lv_event_get_target(e);

    /* Ignore VALUE_CHANGED from LVGL's default handler during custom drag */
    if (slider_drag_state.active_slider == slider) {
        LOG_DBG("Ignoring spurious VALUE_CHANGED during drag");
        return;
    }

    int value = lv_slider_get_value(slider);
    ds_max_layers = (uint8_t)value;

    /* Update value label */
    if (ds_layer_value) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value);
        lv_label_set_text(ds_layer_value, buf);
    }
    LOG_DBG("Max layers: %d", value);
}

/* ========== System Settings Event Handlers ========== */

static void ss_bootloader_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    /* Debug log for all events */
    if (code == LV_EVENT_PRESSED) {
        LOG_INF("Bootloader button: PRESSED");
    } else if (code == LV_EVENT_RELEASED) {
        LOG_INF("Bootloader button: RELEASED");
    }

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        LOG_INF("Bootloader button ACTIVATED - entering bootloader mode");
        /* Use Zephyr 4.x RETENTION_BOOT_MODE API for bootloader entry */
        int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
        if (ret < 0) {
            LOG_ERR("Failed to set bootloader mode: %d", ret);
            return;
        }
        LOG_INF("Bootmode set to BOOTLOADER - rebooting...");
        sys_reboot(SYS_REBOOT_WARM);
    }
}

static void ss_reset_btn_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);

    /* Debug log for all events */
    if (code == LV_EVENT_PRESSED) {
        LOG_INF("Reset button: PRESSED");
    } else if (code == LV_EVENT_RELEASED) {
        LOG_INF("Reset button: RELEASED");
    }

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        LOG_INF("Reset button ACTIVATED - performing system reset");
        sys_reboot(SYS_REBOOT_WARM);
    }
}

/* ========== Display Settings Screen (NO CONTAINER) ========== */

static void destroy_display_settings_widgets(void) {
    LOG_INF("Destroying display settings widgets...");
    if (ds_nav_hint) { lv_obj_del(ds_nav_hint); ds_nav_hint = NULL; }
    if (ds_layer_value) { lv_obj_del(ds_layer_value); ds_layer_value = NULL; }
    if (ds_layer_slider) { lv_obj_del(ds_layer_slider); ds_layer_slider = NULL; }
    if (ds_layer_label) { lv_obj_del(ds_layer_label); ds_layer_label = NULL; }
    if (ds_battery_switch) { lv_obj_del(ds_battery_switch); ds_battery_switch = NULL; }
    if (ds_battery_label) { lv_obj_del(ds_battery_label); ds_battery_label = NULL; }
    if (ds_brightness_value) { lv_obj_del(ds_brightness_value); ds_brightness_value = NULL; }
    if (ds_brightness_slider) { lv_obj_del(ds_brightness_slider); ds_brightness_slider = NULL; }
    if (ds_auto_switch) { lv_obj_del(ds_auto_switch); ds_auto_switch = NULL; }
    if (ds_auto_label) { lv_obj_del(ds_auto_label); ds_auto_label = NULL; }
    if (ds_brightness_label) { lv_obj_del(ds_brightness_label); ds_brightness_label = NULL; }
    if (ds_title_label) { lv_obj_del(ds_title_label); ds_title_label = NULL; }
    LOG_INF("Display settings widgets destroyed");
}

static void create_display_settings_widgets(void) {
    if (!screen_obj) return;
    LOG_INF("Creating display settings widgets (NO CONTAINER)...");

    int y_pos = 15;

    /* Title */
    ds_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_title_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ds_title_label, lv_color_white(), 0);
    lv_label_set_text(ds_title_label, "Display Settings");
    lv_obj_align(ds_title_label, LV_ALIGN_TOP_MID, 0, y_pos);

    y_pos = 50;

    /* ===== Brightness Section ===== */
    ds_brightness_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_brightness_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds_brightness_label, lv_color_white(), 0);
    lv_label_set_text(ds_brightness_label, "Brightness");
    lv_obj_set_pos(ds_brightness_label, 15, y_pos);

    /* Auto label */
    ds_auto_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_auto_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ds_auto_label, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(ds_auto_label, "Auto");
    lv_obj_set_pos(ds_auto_label, 195, y_pos + 4);

    /* Auto switch (iOS style) */
    ds_auto_switch = lv_switch_create(screen_obj);
    lv_obj_set_size(ds_auto_switch, 50, 28);
    lv_obj_set_pos(ds_auto_switch, 230, y_pos);
    if (ds_auto_brightness_enabled) {
        lv_obj_add_state(ds_auto_switch, LV_STATE_CHECKED);
    }
    /* iOS-style switch styling */
    lv_obj_set_style_radius(ds_auto_switch, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ds_auto_switch, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ds_auto_switch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ds_auto_switch, 14, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ds_auto_switch, lv_color_hex(0x34C759), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ds_auto_switch, lv_color_hex(0x3A3A3C), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ds_auto_switch, LV_OPA_COVER, LV_PART_INDICATOR);  /* CRITICAL for visibility */
    lv_obj_set_style_radius(ds_auto_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ds_auto_switch, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ds_auto_switch, LV_OPA_COVER, LV_PART_KNOB);  /* CRITICAL for visibility */
    lv_obj_set_style_pad_all(ds_auto_switch, -2, LV_PART_KNOB);
    lv_obj_set_style_border_width(ds_auto_switch, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(ds_auto_switch, 15);  /* Extend tap area for easier touch */
    lv_obj_add_event_cb(ds_auto_switch, ds_auto_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Disable auto switch if sensor is not available */
    if (!brightness_control_sensor_available()) {
        lv_obj_add_state(ds_auto_switch, LV_STATE_DISABLED);
        lv_obj_set_style_opa(ds_auto_switch, LV_OPA_50, 0);
        lv_label_set_text(ds_auto_label, "Auto (No sensor)");
    }

    y_pos += 35;

    /* Brightness slider (iOS style) */
    ds_brightness_slider = lv_slider_create(screen_obj);
    lv_obj_set_size(ds_brightness_slider, 180, 6);
    lv_obj_set_pos(ds_brightness_slider, 15, y_pos + 8);
    lv_slider_set_range(ds_brightness_slider, 1, 100);
    lv_slider_set_value(ds_brightness_slider, ds_manual_brightness, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(ds_brightness_slider, 20);
    /* iOS-style slider styling */
    lv_obj_set_style_radius(ds_brightness_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ds_brightness_slider, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ds_brightness_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ds_brightness_slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ds_brightness_slider, lv_color_hex(0x007AFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ds_brightness_slider, LV_OPA_COVER, LV_PART_INDICATOR);  /* CRITICAL for visibility */
    lv_obj_set_style_radius(ds_brightness_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ds_brightness_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ds_brightness_slider, LV_OPA_COVER, LV_PART_KNOB);  /* CRITICAL for visibility */
    lv_obj_set_style_pad_all(ds_brightness_slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(ds_brightness_slider, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(ds_brightness_slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(ds_brightness_slider, LV_OPA_30, LV_PART_KNOB);
    if (ds_auto_brightness_enabled) {
        lv_obj_add_state(ds_brightness_slider, LV_STATE_DISABLED);
        lv_obj_set_style_opa(ds_brightness_slider, LV_OPA_50, 0);
    }
    lv_obj_add_event_cb(ds_brightness_slider, ds_brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Custom drag handler for inverted X coordinate */
    lv_obj_add_event_cb(ds_brightness_slider, ds_custom_slider_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ds_brightness_slider, ds_custom_slider_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ds_brightness_slider, ds_custom_slider_drag_cb, LV_EVENT_RELEASED, NULL);

    /* Brightness value label */
    ds_brightness_value = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_brightness_value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds_brightness_value, lv_color_hex(0x007AFF), 0);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", ds_manual_brightness);
    lv_label_set_text(ds_brightness_value, buf);
    lv_obj_set_pos(ds_brightness_value, 230, y_pos);

    y_pos += 40;

    /* ===== Battery Section ===== */
    ds_battery_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_battery_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds_battery_label, lv_color_white(), 0);
    lv_label_set_text(ds_battery_label, "Scanner Battery");
    lv_obj_set_pos(ds_battery_label, 15, y_pos);

    /* Battery switch */
    ds_battery_switch = lv_switch_create(screen_obj);
    lv_obj_set_size(ds_battery_switch, 50, 28);
    lv_obj_set_pos(ds_battery_switch, 230, y_pos - 3);
    if (ds_battery_visible) {
        lv_obj_add_state(ds_battery_switch, LV_STATE_CHECKED);
    }
    /* Same iOS styling */
    lv_obj_set_style_radius(ds_battery_switch, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ds_battery_switch, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ds_battery_switch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ds_battery_switch, 14, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ds_battery_switch, lv_color_hex(0x34C759), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ds_battery_switch, lv_color_hex(0x3A3A3C), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ds_battery_switch, LV_OPA_COVER, LV_PART_INDICATOR);  /* CRITICAL for visibility */
    lv_obj_set_style_radius(ds_battery_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ds_battery_switch, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ds_battery_switch, LV_OPA_COVER, LV_PART_KNOB);  /* CRITICAL for visibility */
    lv_obj_set_style_pad_all(ds_battery_switch, -2, LV_PART_KNOB);
    lv_obj_set_style_border_width(ds_battery_switch, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(ds_battery_switch, 15);  /* Extend tap area for easier touch */
    lv_obj_add_event_cb(ds_battery_switch, ds_battery_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    y_pos += 40;

    /* ===== Max Layers Section ===== */
    ds_layer_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_layer_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds_layer_label, lv_color_white(), 0);
    lv_label_set_text(ds_layer_label, "Max Layers");
    lv_obj_set_pos(ds_layer_label, 15, y_pos);

    y_pos += 25;

    /* Layer slider */
    ds_layer_slider = lv_slider_create(screen_obj);
    lv_obj_set_size(ds_layer_slider, 180, 6);
    lv_obj_set_pos(ds_layer_slider, 15, y_pos + 8);
    lv_slider_set_range(ds_layer_slider, 4, 10);
    lv_slider_set_value(ds_layer_slider, ds_max_layers, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(ds_layer_slider, 20);
    /* Same iOS styling */
    lv_obj_set_style_radius(ds_layer_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ds_layer_slider, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ds_layer_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ds_layer_slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ds_layer_slider, lv_color_hex(0x007AFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ds_layer_slider, LV_OPA_COVER, LV_PART_INDICATOR);  /* CRITICAL for visibility */
    lv_obj_set_style_radius(ds_layer_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ds_layer_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ds_layer_slider, LV_OPA_COVER, LV_PART_KNOB);  /* CRITICAL for visibility */
    lv_obj_set_style_pad_all(ds_layer_slider, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(ds_layer_slider, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(ds_layer_slider, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(ds_layer_slider, LV_OPA_30, LV_PART_KNOB);
    lv_obj_add_event_cb(ds_layer_slider, ds_layer_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* Custom drag handler for inverted X coordinate */
    lv_obj_add_event_cb(ds_layer_slider, ds_custom_slider_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ds_layer_slider, ds_custom_slider_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ds_layer_slider, ds_custom_slider_drag_cb, LV_EVENT_RELEASED, NULL);

    /* Layer value label */
    ds_layer_value = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_layer_value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds_layer_value, lv_color_hex(0x007AFF), 0);
    snprintf(buf, sizeof(buf), "%d", ds_max_layers);
    lv_label_set_text(ds_layer_value, buf);
    lv_obj_set_pos(ds_layer_value, 250, y_pos);

    /* Navigation hint */
    ds_nav_hint = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_nav_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ds_nav_hint, lv_color_hex(0x808080), 0);
    lv_label_set_text(ds_nav_hint, LV_SYMBOL_UP " Main");
    lv_obj_align(ds_nav_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    LOG_INF("Display settings widgets created");
}

/* ========== System Settings Screen (NO CONTAINER) ========== */

static void destroy_system_settings_widgets(void) {
    LOG_INF("Destroying system settings widgets...");
    if (ss_nav_hint) { lv_obj_del(ss_nav_hint); ss_nav_hint = NULL; }
    if (ss_reset_btn) { lv_obj_del(ss_reset_btn); ss_reset_btn = NULL; }
    if (ss_bootloader_btn) { lv_obj_del(ss_bootloader_btn); ss_bootloader_btn = NULL; }
    if (ss_title_label) { lv_obj_del(ss_title_label); ss_title_label = NULL; }
    LOG_INF("System settings widgets destroyed");
}

static void create_system_settings_widgets(void) {
    if (!screen_obj) return;
    LOG_INF("Creating system settings widgets (NO CONTAINER)...");

    /* Title */
    ss_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ss_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ss_title_label, lv_color_white(), 0);
    lv_label_set_text(ss_title_label, "Quick Actions");
    lv_obj_align(ss_title_label, LV_ALIGN_TOP_MID, 0, 20);

    /* Bootloader button (blue) - position matches original system_settings_widget.c */
    ss_bootloader_btn = lv_btn_create(screen_obj);
    lv_obj_set_size(ss_bootloader_btn, 200, 60);
    lv_obj_align(ss_bootloader_btn, LV_ALIGN_CENTER, 0, -15);  /* Original position */
    lv_obj_set_style_bg_color(ss_bootloader_btn, lv_color_hex(0x4A90E2), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ss_bootloader_btn, lv_color_hex(0x357ABD), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(ss_bootloader_btn, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ss_bootloader_btn, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ss_bootloader_btn, lv_color_hex(0x6AAFF0), LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ss_bootloader_btn, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ss_bootloader_btn, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ss_bootloader_btn, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ss_bootloader_btn, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ss_bootloader_btn, LV_OPA_30, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ss_bootloader_btn, 5, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(ss_bootloader_btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ss_bootloader_btn, ss_bootloader_btn_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *bl_label = lv_label_create(ss_bootloader_btn);
    lv_label_set_text(bl_label, "Enter Bootloader");
    lv_obj_set_style_text_font(bl_label, &lv_font_montserrat_18, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(bl_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(bl_label);

    /* Reset button (red) - position matches original system_settings_widget.c */
    ss_reset_btn = lv_btn_create(screen_obj);
    lv_obj_set_size(ss_reset_btn, 200, 60);
    lv_obj_align(ss_reset_btn, LV_ALIGN_CENTER, 0, 55);  /* Original position */
    lv_obj_set_style_bg_color(ss_reset_btn, lv_color_hex(0xE24A4A), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ss_reset_btn, lv_color_hex(0xC93A3A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(ss_reset_btn, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ss_reset_btn, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ss_reset_btn, lv_color_hex(0xF06A6A), LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ss_reset_btn, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ss_reset_btn, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ss_reset_btn, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(ss_reset_btn, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(ss_reset_btn, LV_OPA_30, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ss_reset_btn, 5, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_opa(ss_reset_btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ss_reset_btn, ss_reset_btn_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *rst_label = lv_label_create(ss_reset_btn);
    lv_label_set_text(rst_label, "System Reset");
    lv_obj_set_style_text_font(rst_label, &lv_font_montserrat_18, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(rst_label, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_center(rst_label);

    /* Navigation hint */
    ss_nav_hint = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ss_nav_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ss_nav_hint, lv_color_hex(0x808080), 0);
    lv_label_set_text(ss_nav_hint, LV_SYMBOL_LEFT " Main");
    lv_obj_align(ss_nav_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    LOG_INF("System settings widgets created");
}

/* ========== Keyboard Select Screen Functions ========== */

/* External functions from scanner_stub.c */
extern int scanner_get_selected_keyboard(void);
extern void scanner_set_selected_keyboard(int index);

/* RSSI helper functions (same as original keyboard_list_widget.c) */
static uint8_t ks_rssi_to_bars(int8_t rssi) {
    if (rssi >= -50) return 5;  /* Excellent */
    if (rssi >= -60) return 4;  /* Good */
    if (rssi >= -70) return 3;  /* Fair */
    if (rssi >= -80) return 2;  /* Weak */
    if (rssi >= -90) return 1;  /* Very weak */
    return 0;                   /* No signal */
}

static lv_color_t ks_get_rssi_color(uint8_t bars) {
    if (bars >= 5) return lv_color_hex(0x00CC66);  /* Green */
    if (bars >= 4) return lv_color_hex(0x66CC00);  /* Light green */
    if (bars >= 3) return lv_color_hex(0xFFCC00);  /* Yellow */
    if (bars >= 2) return lv_color_hex(0xFF8800);  /* Orange */
    if (bars >= 1) return lv_color_hex(0xFF3333);  /* Red */
    return lv_color_hex(0x606060);                 /* Gray */
}

/* Keyboard entry click handler */
static void ks_entry_click_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    int keyboard_index = (int)(intptr_t)lv_event_get_user_data(e);
    LOG_INF("Keyboard selected: index=%d", keyboard_index);

    ks_selected_keyboard = keyboard_index;

    /* Update scanner_stub.c to display this keyboard on main screen */
    scanner_set_selected_keyboard(keyboard_index);

    /* Update visual state for all entries */
    for (int i = 0; i < ks_entry_count; i++) {
        struct ks_keyboard_entry *entry = &ks_entries[i];
        if (!entry->container) continue;

        bool is_selected = (entry->keyboard_index == ks_selected_keyboard);
        if (is_selected) {
            lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x2A4A6A), 0);
            lv_obj_set_style_border_color(entry->container, lv_color_hex(0x4A90E2), 0);
            lv_obj_set_style_border_width(entry->container, 2, 0);
        } else {
            lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x1A1A1A), 0);
            lv_obj_set_style_border_color(entry->container, lv_color_hex(0x303030), 0);
            lv_obj_set_style_border_width(entry->container, 1, 0);
        }
    }
}

/* Create a single keyboard entry at absolute position */
static void ks_create_entry(int entry_idx, int y_pos, int keyboard_index,
                            const char *name, int8_t rssi) {
    if (entry_idx >= KS_MAX_KEYBOARDS) return;

    struct ks_keyboard_entry *entry = &ks_entries[entry_idx];
    entry->keyboard_index = keyboard_index;

    /* Clickable container - absolute position */
    entry->container = lv_obj_create(screen_obj);
    lv_obj_set_size(entry->container, 250, 32);
    lv_obj_set_pos(entry->container, 15, y_pos);
    lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(entry->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(entry->container, 1, 0);
    lv_obj_set_style_border_color(entry->container, lv_color_hex(0x303030), 0);
    lv_obj_set_style_radius(entry->container, 6, 0);
    lv_obj_set_style_pad_all(entry->container, 0, 0);
    lv_obj_add_flag(entry->container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(entry->container, ks_entry_click_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)keyboard_index);

    /* Apply selection styling if this is selected */
    if (keyboard_index == ks_selected_keyboard) {
        lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x2A4A6A), 0);
        lv_obj_set_style_border_color(entry->container, lv_color_hex(0x4A90E2), 0);
        lv_obj_set_style_border_width(entry->container, 2, 0);
    }

    /* RSSI bar - absolute position within container */
    entry->rssi_bar = lv_bar_create(entry->container);
    lv_obj_set_size(entry->rssi_bar, 30, 8);
    lv_bar_set_range(entry->rssi_bar, 0, 5);
    uint8_t bars = ks_rssi_to_bars(rssi);
    lv_bar_set_value(entry->rssi_bar, bars, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(entry->rssi_bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(entry->rssi_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(entry->rssi_bar, ks_get_rssi_color(bars), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(entry->rssi_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(entry->rssi_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(entry->rssi_bar, 2, LV_PART_INDICATOR);
    lv_obj_align(entry->rssi_bar, LV_ALIGN_LEFT_MID, 8, 0);

    /* RSSI label */
    entry->rssi_label = lv_label_create(entry->container);
    char rssi_buf[16];
    snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", rssi);
    lv_label_set_text(entry->rssi_label, rssi_buf);
    lv_obj_set_style_text_color(entry->rssi_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(entry->rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_align(entry->rssi_label, LV_ALIGN_LEFT_MID, 42, 0);

    /* Keyboard name */
    entry->name_label = lv_label_create(entry->container);
    lv_label_set_text(entry->name_label, name);
    lv_obj_set_style_text_color(entry->name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(entry->name_label, &lv_font_montserrat_16, 0);
    lv_obj_align(entry->name_label, LV_ALIGN_LEFT_MID, 100, 0);

    LOG_DBG("Created keyboard entry %d: %s (rssi=%d)", entry_idx, name, rssi);
}

/* Destroy a single keyboard entry */
static void ks_destroy_entry(struct ks_keyboard_entry *entry) {
    if (entry->container) {
        lv_obj_del(entry->container);  /* Deletes all children too */
        entry->container = NULL;
    }
    entry->rssi_bar = NULL;
    entry->rssi_label = NULL;
    entry->name_label = NULL;
    entry->keyboard_index = -1;
}

/* Update keyboard entries with current scanner data */
static void ks_update_entries(void) {
    /* Count active keyboards */
    int active_keyboards[KS_MAX_KEYBOARDS];
    int active_count = 0;

    for (int i = 0; i < CONFIG_PROSPECTOR_MAX_KEYBOARDS && active_count < KS_MAX_KEYBOARDS; i++) {
        struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
        if (kbd && kbd->active) {
            active_keyboards[active_count++] = i;
        }
    }

    /* Auto-select first keyboard if none selected */
    if (ks_selected_keyboard < 0 && active_count > 0) {
        ks_selected_keyboard = active_keyboards[0];
        LOG_INF("Auto-selected keyboard index %d", ks_selected_keyboard);
    }

    /* Check if selected keyboard is still active */
    if (ks_selected_keyboard >= 0 && active_count > 0) {
        bool found = false;
        for (int i = 0; i < active_count; i++) {
            if (active_keyboards[i] == ks_selected_keyboard) {
                found = true;
                break;
            }
        }
        if (!found) {
            ks_selected_keyboard = active_keyboards[0];
            LOG_INF("Selected keyboard lost, switched to index %d", ks_selected_keyboard);
        }
    }

    /* Recreate entries if count changed */
    if (active_count != ks_entry_count) {
        LOG_INF("Keyboard count changed: %d -> %d", ks_entry_count, active_count);

        /* Destroy all existing entries */
        for (int i = 0; i < ks_entry_count; i++) {
            ks_destroy_entry(&ks_entries[i]);
        }
        ks_entry_count = 0;

        /* Create new entries */
        int y_pos = 55;  /* Start below title */
        int spacing = 40;

        for (int i = 0; i < active_count; i++) {
            int kbd_idx = active_keyboards[i];
            struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(kbd_idx);
            if (!kbd) continue;

            const char *name = kbd->ble_name[0] ? kbd->ble_name : "Unknown";
            ks_create_entry(i, y_pos + (i * spacing), kbd_idx, name, kbd->rssi);
        }
        ks_entry_count = active_count;
    } else {
        /* Just update existing entries */
        int entry_idx = 0;
        for (int i = 0; i < CONFIG_PROSPECTOR_MAX_KEYBOARDS && entry_idx < ks_entry_count; i++) {
            struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
            if (!kbd || !kbd->active) continue;

            struct ks_keyboard_entry *entry = &ks_entries[entry_idx];
            if (!entry->container) {
                entry_idx++;
                continue;
            }

            /* Update name */
            const char *name = kbd->ble_name[0] ? kbd->ble_name : "Unknown";
            lv_label_set_text(entry->name_label, name);

            /* Update RSSI */
            uint8_t bars = ks_rssi_to_bars(kbd->rssi);
            lv_bar_set_value(entry->rssi_bar, bars, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(entry->rssi_bar, ks_get_rssi_color(bars), LV_PART_INDICATOR);
            char rssi_buf[16];
            snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", kbd->rssi);
            lv_label_set_text(entry->rssi_label, rssi_buf);

            /* Update selection styling */
            bool is_selected = (entry->keyboard_index == ks_selected_keyboard);
            if (is_selected) {
                lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x2A4A6A), 0);
                lv_obj_set_style_border_color(entry->container, lv_color_hex(0x4A90E2), 0);
                lv_obj_set_style_border_width(entry->container, 2, 0);
            } else {
                lv_obj_set_style_bg_color(entry->container, lv_color_hex(0x1A1A1A), 0);
                lv_obj_set_style_border_color(entry->container, lv_color_hex(0x303030), 0);
                lv_obj_set_style_border_width(entry->container, 1, 0);
            }

            entry_idx++;
        }
    }
}

/* LVGL timer callback for periodic updates */
static void ks_update_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    if (transition_in_progress || ui_interaction_active) {
        return;
    }

    ks_update_entries();
}

static void destroy_keyboard_select_widgets(void) {
    LOG_INF("Destroying keyboard select widgets...");

    /* Stop update timer */
    if (ks_update_timer) {
        lv_timer_del(ks_update_timer);
        ks_update_timer = NULL;
    }

    /* Destroy all keyboard entries */
    for (int i = 0; i < ks_entry_count; i++) {
        ks_destroy_entry(&ks_entries[i]);
    }
    ks_entry_count = 0;

    /* Destroy other widgets */
    if (ks_nav_hint) { lv_obj_del(ks_nav_hint); ks_nav_hint = NULL; }
    if (ks_title_label) { lv_obj_del(ks_title_label); ks_title_label = NULL; }

    LOG_INF("Keyboard select widgets destroyed");
}

static void create_keyboard_select_widgets(void) {
    LOG_INF("Creating keyboard select widgets...");

    /* Get current selection from scanner_stub.c */
    ks_selected_keyboard = scanner_get_selected_keyboard();
    LOG_INF("Current selected keyboard: %d", ks_selected_keyboard);

    /* Title */
    ks_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ks_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ks_title_label, lv_color_white(), 0);
    lv_label_set_text(ks_title_label, "Select Keyboard");
    lv_obj_align(ks_title_label, LV_ALIGN_TOP_MID, 0, 15);

    /* Navigation hint */
    ks_nav_hint = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ks_nav_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ks_nav_hint, lv_color_hex(0x808080), 0);
    lv_label_set_text(ks_nav_hint, LV_SYMBOL_DOWN " Main");
    lv_obj_align(ks_nav_hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* Create initial keyboard entries */
    ks_update_entries();

    /* Start update timer (1 second interval) */
    ks_update_timer = lv_timer_create(ks_update_timer_cb, 1000, NULL);

    LOG_INF("Keyboard select widgets created (%d keyboards)", ks_entry_count);
}

/* ========== Swipe Processing (runs in LVGL timer = Main Thread) ========== */

/**
 * Helper: Register LVGL input device (call once when first settings screen shown)
 */
static void ensure_lvgl_indev_registered(void) {
    if (!lvgl_indev_registered) {
        LOG_INF("Registering LVGL input device for touch interactions...");
        int ret = touch_handler_register_lvgl_indev();
        if (ret == 0) {
            lvgl_indev_registered = true;
            LOG_INF("LVGL input device registered successfully");
        } else {
            LOG_ERR("Failed to register LVGL input device: %d", ret);
        }
    }
}

/**
 * Process pending swipe in main thread context (LVGL timer callback)
 * This ensures all LVGL operations are thread-safe.
 *
 * DESIGN PRINCIPLE (from CLAUDE.md):
 * - ISR/Callback から LVGL API を呼ばない - フラグを立てるだけ
 * - すべての処理は Main Task で実行
 *
 * SCREEN NAVIGATION (visual finger direction):
 * - Main Screen → DOWN → Display Settings
 * - Main Screen → UP → Keyboard Select
 * - Main Screen → RIGHT → Quick Actions
 * - Display Settings → UP → Main Screen
 * - Keyboard Select → DOWN → Main Screen
 * - Quick Actions → LEFT → Main Screen
 *
 * Coordinate transform corrected - swipe directions now match user's physical gesture
 */
static void swipe_process_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    /* Check for pending swipe */
    enum swipe_direction dir = pending_swipe;
    if (dir == SWIPE_DIRECTION_NONE) {
        return;  /* No pending swipe */
    }

    /* Clear pending flag immediately (atomic read-then-clear) */
    pending_swipe = SWIPE_DIRECTION_NONE;

    /* Skip if UI interaction in progress (slider dragging) */
    if (ui_interaction_active) {
        LOG_DBG("Swipe ignored - UI interaction in progress");
        return;
    }

    /* Skip if already transitioning */
    if (transition_in_progress) {
        LOG_WRN("Swipe ignored - transition already in progress");
        return;
    }

    LOG_INF("[MAIN THREAD] Processing swipe: direction=%d, current_screen=%d",
            dir, current_screen);

    /* Set transition flag to protect against concurrent operations */
    transition_in_progress = true;

    switch (dir) {
    case SWIPE_DIRECTION_DOWN:
        /* Main → Display Settings OR Keyboard Select → Main */
        if (current_screen == SCREEN_MAIN) {
            LOG_INF(">>> Transitioning: MAIN -> DISPLAY_SETTINGS");
            destroy_main_screen_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_hex(0x0A0A0A), 0);
            lv_obj_invalidate(screen_obj);
            create_display_settings_widgets();
            ensure_lvgl_indev_registered();  /* Register for slider/switch touch */
            current_screen = SCREEN_DISPLAY_SETTINGS;
            LOG_INF(">>> Transition complete");
        } else if (current_screen == SCREEN_KEYBOARD_SELECT) {
            LOG_INF(">>> Transitioning: KEYBOARD_SELECT -> MAIN");
            destroy_keyboard_select_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
            lv_obj_invalidate(screen_obj);
            create_main_screen_widgets();
            current_screen = SCREEN_MAIN;
            LOG_INF(">>> Transition complete");
        }
        break;

    case SWIPE_DIRECTION_UP:
        /* Display Settings → Main OR Main → Keyboard Select */
        if (current_screen == SCREEN_DISPLAY_SETTINGS) {
            LOG_INF(">>> Transitioning: DISPLAY_SETTINGS -> MAIN");
            destroy_display_settings_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
            lv_obj_invalidate(screen_obj);
            create_main_screen_widgets();
            current_screen = SCREEN_MAIN;
            LOG_INF(">>> Transition complete");
        } else if (current_screen == SCREEN_MAIN) {
            LOG_INF(">>> Transitioning: MAIN -> KEYBOARD_SELECT");
            destroy_main_screen_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_hex(0x0A0A0A), 0);
            lv_obj_invalidate(screen_obj);
            create_keyboard_select_widgets();
            ensure_lvgl_indev_registered();  /* Register for keyboard entry touch */
            current_screen = SCREEN_KEYBOARD_SELECT;
            LOG_INF(">>> Transition complete");
        }
        break;

    case SWIPE_DIRECTION_LEFT:
        /* Quick Actions → Main (user swipes left) */
        if (current_screen == SCREEN_SYSTEM_SETTINGS) {
            LOG_INF(">>> Transitioning: QUICK_ACTIONS -> MAIN");
            destroy_system_settings_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
            lv_obj_invalidate(screen_obj);
            create_main_screen_widgets();
            current_screen = SCREEN_MAIN;
            LOG_INF(">>> Transition complete");
        }
        break;

    case SWIPE_DIRECTION_RIGHT:
        /* Main → Quick Actions (user swipes right) */
        if (current_screen == SCREEN_MAIN) {
            LOG_INF(">>> Transitioning: MAIN -> QUICK_ACTIONS");
            destroy_main_screen_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_hex(0x0A0A0A), 0);
            lv_obj_invalidate(screen_obj);
            create_system_settings_widgets();
            ensure_lvgl_indev_registered();  /* Register for button touch */
            current_screen = SCREEN_SYSTEM_SETTINGS;
            LOG_INF(">>> Transition complete");
        }
        break;

    default:
        LOG_DBG("Swipe direction not handled for current screen: dir=%d, screen=%d",
                dir, current_screen);
        break;
    }

    /* Clear transition flag */
    transition_in_progress = false;
}

/* ========== Swipe Event Handler (runs in ISR context - just set flag!) ========== */

/**
 * ZMK event listener - runs synchronously in the thread that raises the event.
 * Since touch_handler raises events from INPUT thread (ISR context),
 * this listener ALSO runs in ISR context!
 *
 * CRITICAL: Do NOT call LVGL APIs here! Just set a flag for main thread processing.
 */
static int swipe_gesture_listener(const zmk_event_t *eh) {
    const struct zmk_swipe_gesture_event *ev = as_zmk_swipe_gesture_event(eh);
    if (ev == NULL) return ZMK_EV_EVENT_BUBBLE;

    /* Skip if already have pending swipe (debounce) */
    if (pending_swipe != SWIPE_DIRECTION_NONE) {
        LOG_DBG("Swipe queued - already have pending swipe");
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("[ISR] Swipe event received: direction=%d (queuing for main thread)",
            ev->direction);

    /* Just set the flag - processing happens in LVGL timer (main thread) */
    pending_swipe = ev->direction;

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(swipe_gesture, swipe_gesture_listener);
ZMK_SUBSCRIPTION(swipe_gesture, zmk_swipe_gesture_event);
