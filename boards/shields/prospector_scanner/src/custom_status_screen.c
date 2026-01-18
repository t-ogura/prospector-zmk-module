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

/* ========== Pending Display Data from scanner_stub.c ========== */
/* Work queue sets data + flag, LVGL timer here processes it in main thread */
#define MAX_NAME_LEN 32
struct pending_display_data {
    volatile bool update_pending;
    volatile bool signal_update_pending;  /* Signal widget updates separately (1Hz) */
    volatile bool no_keyboards;           /* True when all keyboards timed out */
    char device_name[MAX_NAME_LEN];
    int layer;
    int wpm;
    bool usb_ready;
    bool ble_connected;
    bool ble_bonded;
    int profile;
    uint8_t modifiers;
    int bat[4];
    int8_t rssi;
    float rate_hz;
    int scanner_battery;
    bool scanner_battery_pending;
};

/* Defined in scanner_stub.c */
extern bool scanner_get_pending_update(struct pending_display_data *out);
extern bool scanner_is_signal_pending(void);
extern volatile int8_t scanner_signal_rssi;
extern volatile int32_t scanner_signal_rate_x100;  /* rate * 100 */
extern bool scanner_get_pending_battery(int *level);

/* LVGL timer for processing pending updates in main thread */
static lv_timer_t *pending_update_timer = NULL;

/* ========== Screen State Management ========== */
enum screen_state {
    SCREEN_MAIN = 0,
    SCREEN_DISPLAY_SETTINGS,
    SCREEN_SYSTEM_SETTINGS,
    SCREEN_KEYBOARD_SELECT,
    SCREEN_PONG_WARS,
};

static enum screen_state current_screen = SCREEN_MAIN;
static lv_obj_t *screen_obj = NULL;

/* Transition protection flag - checked by work queues */
volatile bool transition_in_progress = false;

/* Pong Wars active flag - stops all background display updates */
volatile bool pong_wars_active = false;

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
static void destroy_pong_wars_widgets(void);
static void create_pong_wars_widgets(void);
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
#define MAX_KB_BATTERIES 4
static int battery_values[MAX_KB_BATTERIES] = {0, 0, 0, 0};  /* Up to 4 keyboard batteries */
static int active_battery_count = 0;  /* How many batteries are active (>0) */
static int scanner_battery = 0;
static int8_t rssi = -100;  /* Default: very weak signal */
static float rate_hz = -1.0f;  /* Negative = not yet received, will show as "-.--Hz" */
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

/* Layer - Fixed mode */
static lv_obj_t *layer_title_label = NULL;
static lv_obj_t *layer_labels[10] = {NULL};
static lv_obj_t *layer_over_max_label = NULL;  /* Large number for over-max display */
static bool layer_mode_over_max = false;       /* true when active_layer >= max_layers */
static int last_active_layer = -1;             /* Track previous layer for animations */

/* Layer - Slide mode */
#define SLIDE_VISIBLE_COUNT 9   /* Number of visible layer slots: 小中大大大大大中小 */
#define SLIDE_LARGE_COUNT 3     /* Number of "large" slots in center */
static lv_obj_t *layer_slide_labels[SLIDE_VISIBLE_COUNT] = {NULL};
static int layer_slide_window_start = 0;  /* First visible layer number in the window */

/* Modifier - placeholder for now (no NerdFont in ZMK test) */
static lv_obj_t *modifier_label = NULL;

/* Keyboard battery - array for up to 4 batteries */
static lv_obj_t *kb_bat_bar[MAX_KB_BATTERIES] = {NULL};      /* Battery bars (connected) */
static lv_obj_t *kb_bat_pct[MAX_KB_BATTERIES] = {NULL};      /* Percentage labels */
static lv_obj_t *kb_bat_name[MAX_KB_BATTERIES] = {NULL};     /* Name labels (L, R, Aux, A1, A2) */
static lv_obj_t *kb_bat_nc_bar[MAX_KB_BATTERIES] = {NULL};   /* Disconnected state bars */
static lv_obj_t *kb_bat_nc_label[MAX_KB_BATTERIES] = {NULL}; /* Disconnected state × symbols */

/* Battery name labels based on count:
 * 1: (no label)
 * 2: "L", "R"
 * 3: "L", "R", "Aux"
 * 4: "L", "R", "A1", "A2"
 */
static const char *battery_names_2[] = {"L", "R", NULL, NULL};
static const char *battery_names_3[] = {"L", "R", "Aux", NULL};
static const char *battery_names_4[] = {"L", "R", "A1", "A2"};

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
static lv_obj_t *ds_slide_label = NULL;
static lv_obj_t *ds_slide_switch = NULL;
static lv_obj_t *ds_nav_hint = NULL;

/* Display Settings State (persists across screen transitions) */
static bool ds_auto_brightness_enabled = false;
static uint8_t ds_manual_brightness = 65;
/* Battery visible if CONFIG_PROSPECTOR_BATTERY_SUPPORT=y in config */
static bool ds_battery_visible = IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT);
static uint8_t ds_max_layers = 7;
static bool ds_layer_slide_mode = IS_ENABLED(CONFIG_PROSPECTOR_LAYER_SLIDE_DEFAULT);  /* Slide animation mode for layer display */
static uint8_t ds_layer_slide_max = 7;    /* Dynamic max layer for slide mode */

/* Forward declarations for layer display helpers */
static void create_layer_list_widgets(lv_obj_t *parent, int y_offset);
static void destroy_layer_list_widgets(void);
static void create_over_max_widget(lv_obj_t *parent, int layer, int y_offset);
static void destroy_over_max_widget(void);
/* Slide mode helpers */
static void create_layer_slide_widgets(lv_obj_t *parent, int y_offset);
static void destroy_layer_slide_widgets(void);
static void update_layer_slide_display(int layer, bool animate);
static lv_color_t get_slide_layer_color(int layer, int max_layer);

/* UI interaction flag - prevents swipe during slider drag */
static bool ui_interaction_active = false;

/* LVGL input device registration flag - register once when first settings screen shown */
static bool lvgl_indev_registered = false;

/* ========== System Settings Screen Widgets (NO CONTAINER) ========== */
static lv_obj_t *ss_title_label = NULL;
static lv_obj_t *ss_version_label = NULL;
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
    lv_obj_t *channel_badge; /* Channel number badge */
    int keyboard_index;      /* Index in scanner's keyboard array */
};
static struct ks_keyboard_entry ks_entries[KS_MAX_KEYBOARDS] = {0};
static uint8_t ks_entry_count = 0;

/* Channel selector UI in keyboard select header */
static lv_obj_t *ks_channel_container = NULL;  /* Tappable channel display */
static lv_obj_t *ks_channel_value = NULL;

/* Channel popup UI */
static lv_obj_t *ks_channel_popup = NULL;
static lv_obj_t *ks_channel_popup_btns[11] = {NULL};  /* 0-9 + All(10) */

/* Runtime channel (defined in system_settings_widget.c, fallback here) */
/* Default to CHANNEL_ALL (10) = show all keyboards */
static uint8_t ks_runtime_channel = 10;  /* CHANNEL_ALL */
static bool ks_channel_initialized = false;

/* Channel color palette (pastel colors for good visibility) */
static lv_color_t get_channel_color(uint8_t channel) {
    switch (channel) {
        case 1: return lv_color_hex(0xFF6B6B);  /* Red */
        case 2: return lv_color_hex(0xFFA94D);  /* Orange */
        case 3: return lv_color_hex(0xFFE066);  /* Yellow */
        case 4: return lv_color_hex(0x69DB7C);  /* Green */
        case 5: return lv_color_hex(0x4DABF7);  /* Blue */
        case 6: return lv_color_hex(0xB197FC);  /* Purple */
        case 7: return lv_color_hex(0xF783AC);  /* Pink */
        case 8: return lv_color_hex(0x66D9E8);  /* Cyan */
        case 9: return lv_color_hex(0xDEE2E6);  /* Gray */
        default: return lv_color_hex(0x808080); /* Default gray for Ch0/All */
    }
}

/* Channel functions - try to use system_settings_widget.c version if available */
__attribute__((weak)) uint8_t scanner_get_runtime_channel(void) {
    if (!ks_channel_initialized) {
        ks_runtime_channel = 10;  /* Default: All (CHANNEL_ALL=10) */
        ks_channel_initialized = true;
    }
    return ks_runtime_channel;
}

__attribute__((weak)) void scanner_set_runtime_channel(uint8_t channel) {
    ks_runtime_channel = channel;
    ks_channel_initialized = true;
    LOG_INF("Channel set to %d", channel);
}

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

/* Dynamic Hue-based pastel color for slide mode
 * Hue is divided evenly by max_layer count
 * Returns pastel color with 40% saturation, 100% brightness */
static lv_color_t get_slide_layer_color(int layer, int max_layer) {
    if (max_layer <= 0) max_layer = 1;

    /* Calculate Hue (0-360) based on layer position */
    int hue = (layer * 360) / max_layer;
    hue = hue % 360;  /* Wrap around */

    /* HSV to RGB conversion with S=0.4 (pastel), V=1.0 (bright) */
    float s = 0.4f;
    float v = 1.0f;
    float h = hue / 60.0f;
    int i = (int)h;
    float f = h - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));

    float r, g, b;
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }

    return lv_color_make((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
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

/* ========== Pending Update Timer Callback (runs in main thread) ========== */
static char last_keyboard_name[MAX_NAME_LEN] = "";  /* Track keyboard changes */

static void pending_update_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    /* Only process updates on main screen */
    if (current_screen != SCREEN_MAIN) {
        return;
    }

    /* Check for pending display update */
    struct pending_display_data data;
    if (scanner_get_pending_update(&data)) {
        /* Check if all keyboards have timed out */
        if (data.no_keyboards) {
            LOG_INF("All keyboards timed out - returning to Scanning... state");

            /* Reset display to initial "Scanning..." state */
            display_update_device_name("Scanning...");
            display_update_layer(0);
            display_update_wpm(0);
            display_update_connection(false, false, false, 0);
            display_update_modifiers(0);
            display_update_keyboard_battery_4(0, 0, 0, 0);

            /* Clear last keyboard name so next keyboard triggers battery reposition */
            last_keyboard_name[0] = '\0';
            active_battery_count = -1;

            /* Apply timeout brightness if configured */
#ifdef CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS
            if (CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS > 0) {
                set_pwm_brightness(CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS);
                LOG_INF("Timeout brightness set to %d%%", CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS);
            }
#endif
            return;
        }

        /* Detect keyboard change - reset battery count to force full reposition */
        if (strcmp(last_keyboard_name, data.device_name) != 0) {
            LOG_INF("Keyboard changed: %s -> %s, resetting battery layout",
                    last_keyboard_name, data.device_name);
            strncpy(last_keyboard_name, data.device_name, MAX_NAME_LEN - 1);
            last_keyboard_name[MAX_NAME_LEN - 1] = '\0';
            active_battery_count = -1;  /* Force reposition on next battery update */

            /* Restore normal brightness when keyboard activity resumes */
#ifdef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS
            set_pwm_brightness(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
            LOG_INF("Brightness restored to %d%%", CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
#endif
        }

        /* Process all updates in main thread - safe to call LVGL */
        display_update_device_name(data.device_name);
        display_update_layer(data.layer);
        display_update_wpm(data.wpm);
        display_update_connection(data.usb_ready, data.ble_connected,
                                  data.ble_bonded, data.profile);
        display_update_modifiers(data.modifiers);

        /* Battery update */
        if (data.bat[1] == 0 && data.bat[2] == 0 && data.bat[3] == 0) {
            display_update_keyboard_battery_4(data.bat[0], 0, 0, 0);
        } else {
            display_update_keyboard_battery_4(data.bat[0], data.bat[1],
                                              data.bat[2], data.bat[3]);
        }
    }

    /* Check for pending signal update (separate from main data, updates at 1Hz) */
    /* Read globals directly and update display inline (avoid ALL float function params) */
    if (scanner_is_signal_pending()) {
        int8_t sig_rssi = scanner_signal_rssi;
        int32_t sig_rate_x100 = scanner_signal_rate_x100;

        /* Update signal display INLINE (no function call with float param) */
        rssi = sig_rssi;
        rate_hz = (float)sig_rate_x100 / 100.0f;

        uint8_t bars = rssi_to_bars(sig_rssi);
        if (rssi_bar) {
            lv_bar_set_value(rssi_bar, bars, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(rssi_bar, get_rssi_color(bars), LV_PART_INDICATOR);
        }
        if (rssi_label) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%ddBm", sig_rssi);
            lv_label_set_text(rssi_label, buf);
        }
        if (rate_label) {
            char buf[16];
            if (sig_rate_x100 < 0) {
                snprintf(buf, sizeof(buf), "-.--Hz");
            } else {
                /* Display from integer directly: rate_x100 / 100 . rate_x100 % 100 */
                int whole = sig_rate_x100 / 100;
                int frac = (sig_rate_x100 % 100) / 10;  /* One decimal place */
                snprintf(buf, sizeof(buf), "%d.%dHz", whole, frac);
            }
            lv_label_set_text(rate_label, buf);
        }
    }

    /* Check for pending scanner battery update */
    int scanner_bat;
    if (scanner_get_pending_battery(&scanner_bat)) {
        display_update_scanner_battery(scanner_bat);
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

    /* Hide battery widget if disabled */
    if (!ds_battery_visible) {
        lv_obj_set_style_opa(scanner_bat_icon, 0, 0);
        lv_obj_set_style_opa(scanner_bat_pct, 0, 0);
    }
    LOG_INF("[INIT] scanner battery created (visible=%d)", ds_battery_visible);

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

    /* Create layer display - slide mode OR fixed mode (list/over-max) */
    if (ds_layer_slide_mode) {
        /* Slide mode: create 7-slot dial display */
        create_layer_slide_widgets(screen, 105);
        layer_mode_over_max = false;  /* Not used in slide mode */
    } else if (active_layer >= ds_max_layers) {
        layer_mode_over_max = true;
        create_over_max_widget(screen, active_layer, 105);
    } else {
        layer_mode_over_max = false;
        create_layer_list_widgets(screen, 105);
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

    /* ===== 7. Keyboard Battery (dynamic layout for 1-4 batteries) ===== */
    LOG_INF("[INIT] Creating keyboard battery widgets...");

    /* Position constants - configurable for different battery counts */
    #define KB_BAR_HEIGHT      4
    #define KB_BAR_Y_OFFSET    -33    /* Distance from bottom */
    #define KB_PCT_Y_OFFSET    -42    /* Percentage label above bar */
    #define KB_NAME_X_OFFSET   0      /* Name label right edge aligns with bar left edge */

    /* Layout for different battery counts (bar width and positions) */
    /* 1 battery: centered, width 165 (1.5x of 110) */
    /* 2 batteries: L/R side by side, width 110 each */
    /* 3 batteries: width 70 each, spread across */
    /* 4 batteries: width 52 each, spread across */
    #define KB_BAR_WIDTH_1     165
    #define KB_BAR_WIDTH_2     110
    #define KB_BAR_WIDTH_3     70
    #define KB_BAR_WIDTH_4     52

    /* X offsets for each layout (from center) */
    static const int16_t kb_x_offsets_1[] = {0};
    static const int16_t kb_x_offsets_2[] = {-70, 70};
    static const int16_t kb_x_offsets_3[] = {-90, 0, 90};
    static const int16_t kb_x_offsets_4[] = {-100, -35, 35, 100};

    /* Create all 4 battery slot widgets (initially hidden) */
    for (int i = 0; i < MAX_KB_BATTERIES; i++) {
        /* Default to 2-battery layout initially */
        int16_t bar_width = KB_BAR_WIDTH_2;
        int16_t x_offset = (i < 2) ? kb_x_offsets_2[i] : 0;

        /* Connected state bar */
        kb_bat_bar[i] = lv_bar_create(screen);
        lv_obj_set_size(kb_bat_bar[i], bar_width, KB_BAR_HEIGHT);
        lv_obj_align(kb_bat_bar[i], LV_ALIGN_BOTTOM_MID, x_offset, KB_BAR_Y_OFFSET);
        lv_bar_set_range(kb_bat_bar[i], 0, 100);
        lv_bar_set_value(kb_bat_bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(kb_bat_bar[i], lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(kb_bat_bar[i], 255, LV_PART_MAIN);
        lv_obj_set_style_radius(kb_bat_bar[i], 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(kb_bat_bar[i], lv_color_hex(0x909090), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(kb_bat_bar[i], 255, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(kb_bat_bar[i], lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(kb_bat_bar[i], LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
        lv_obj_set_style_radius(kb_bat_bar[i], 1, LV_PART_INDICATOR);
        lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_MAIN);
        lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_INDICATOR);

        /* Percentage label (above bar, centered) */
        kb_bat_pct[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(kb_bat_pct[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(kb_bat_pct[i], lv_color_white(), 0);
        lv_obj_align(kb_bat_pct[i], LV_ALIGN_BOTTOM_MID, x_offset, KB_PCT_Y_OFFSET);
        lv_label_set_text(kb_bat_pct[i], "0");
        lv_obj_set_style_opa(kb_bat_pct[i], 0, 0);

        /* Name label (left of bar, same height as percentage) */
        kb_bat_name[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(kb_bat_name[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(kb_bat_name[i], lv_color_hex(0x808080), 0);
        lv_obj_align(kb_bat_name[i], LV_ALIGN_BOTTOM_MID, x_offset - bar_width/2 + KB_NAME_X_OFFSET, KB_PCT_Y_OFFSET);
        lv_obj_set_style_text_align(kb_bat_name[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(kb_bat_name[i], "");
        lv_obj_set_style_opa(kb_bat_name[i], 0, 0);

        /* Disconnected state bar */
        kb_bat_nc_bar[i] = lv_obj_create(screen);
        lv_obj_set_size(kb_bat_nc_bar[i], bar_width, KB_BAR_HEIGHT);
        lv_obj_align(kb_bat_nc_bar[i], LV_ALIGN_BOTTOM_MID, x_offset, KB_BAR_Y_OFFSET);
        lv_obj_set_style_bg_color(kb_bat_nc_bar[i], lv_color_hex(0x9e2121), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(kb_bat_nc_bar[i], 255, LV_PART_MAIN);
        lv_obj_set_style_radius(kb_bat_nc_bar[i], 1, LV_PART_MAIN);
        lv_obj_set_style_border_width(kb_bat_nc_bar[i], 0, 0);
        lv_obj_set_style_pad_all(kb_bat_nc_bar[i], 0, 0);
        /* Initially hide slots 2 and 3 (only show first 2 by default) */
        lv_obj_set_style_opa(kb_bat_nc_bar[i], (i < 2) ? 255 : 0, 0);

        /* Disconnected state label (× symbol) */
        kb_bat_nc_label[i] = lv_label_create(screen);
        lv_obj_set_style_text_font(kb_bat_nc_label[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(kb_bat_nc_label[i], lv_color_hex(0xe63030), 0);
        lv_obj_align(kb_bat_nc_label[i], LV_ALIGN_BOTTOM_MID, x_offset, KB_PCT_Y_OFFSET);
        lv_label_set_text(kb_bat_nc_label[i], LV_SYMBOL_CLOSE);
        lv_obj_set_style_opa(kb_bat_nc_label[i], (i < 2) ? 255 : 0, 0);
    }

    LOG_INF("[INIT] keyboard battery widgets created (4 slots)");

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

    /* Create pending update timer - processes Work Queue data in main thread */
    if (!pending_update_timer) {
        pending_update_timer = lv_timer_create(pending_update_timer_cb, 100, NULL);
        LOG_INF("Pending update timer registered (100ms interval)");
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

/* Animation callback for horizontal (X) slide - for over-max label (uses align) */
static void layer_slide_x_anim_cb(void *var, int32_t value) {
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_align(obj, LV_ALIGN_TOP_MID, value, 105);  /* Keep Y at 105, animate X offset */
}

/* Animation callback for absolute X position - for layer list labels */
static void layer_pos_x_anim_cb(void *var, int32_t value) {
    lv_obj_set_x((lv_obj_t *)var, value);
}

/* Animation callback for pulse scale effect */
static void layer_pulse_anim_cb(void *var, int32_t value) {
    /* value goes 100 -> 130 -> 100 (percentage) */
    lv_obj_t *label = (lv_obj_t *)var;
    int32_t scale = (value * 256) / 100;  /* Convert to LVGL scale (256 = 100%) */
    lv_obj_set_style_transform_scale(label, scale, 0);
}

/* Helper to create layer list widgets */
static void create_layer_list_widgets(lv_obj_t *parent, int y_offset) {
    int num_layers = ds_max_layers;
    int spacing = 25;
    int label_width = 22;
    int start_x = 140 - ((num_layers - 1) * spacing / 2) - (label_width / 2);

    for (int i = 0; i < num_layers && i < 10; i++) {
        layer_labels[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(layer_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_set_width(layer_labels[i], label_width);
        lv_obj_set_style_text_align(layer_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        /* Enable transform for pulse animation */
        lv_obj_set_style_transform_pivot_x(layer_labels[i], label_width / 2, 0);
        lv_obj_set_style_transform_pivot_y(layer_labels[i], 14, 0);  /* Half of font height */

        char text[4];
        snprintf(text, sizeof(text), "%d", i);
        lv_label_set_text(layer_labels[i], text);

        if (i == active_layer) {
            lv_obj_set_style_text_color(layer_labels[i], get_layer_color(i), 0);
            lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_color(layer_labels[i], lv_color_make(40, 40, 40), 0);
            lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_30, 0);
        }
        lv_obj_set_pos(layer_labels[i], start_x + (i * spacing), y_offset);
    }
}

/* Helper to destroy layer list widgets */
static void destroy_layer_list_widgets(void) {
    for (int i = 0; i < 10; i++) {
        if (layer_labels[i]) {
            lv_obj_del(layer_labels[i]);
            layer_labels[i] = NULL;
        }
    }
}

/* Helper to create over-max label widget */
static void create_over_max_widget(lv_obj_t *parent, int layer, int y_offset) {
    layer_over_max_label = lv_label_create(parent);
    lv_obj_set_style_text_font(layer_over_max_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(layer_over_max_label, get_layer_color(layer % 10), 0);
    lv_obj_set_style_text_align(layer_over_max_label, LV_TEXT_ALIGN_CENTER, 0);
    /* Enable transform for animations */
    lv_obj_set_style_transform_pivot_x(layer_over_max_label, 30, 0);
    lv_obj_set_style_transform_pivot_y(layer_over_max_label, 14, 0);

    char text[8];
    snprintf(text, sizeof(text), "%d", layer);
    lv_label_set_text(layer_over_max_label, text);
    lv_obj_align(layer_over_max_label, LV_ALIGN_TOP_MID, 0, y_offset);
}

/* Helper to destroy over-max widget */
static void destroy_over_max_widget(void) {
    if (layer_over_max_label) {
        lv_obj_del(layer_over_max_label);
        layer_over_max_label = NULL;
    }
}

/* Callback to delete object after slide-out animation completes */
static void slide_out_ready_cb(lv_anim_t *anim) {
    lv_obj_t *obj = (lv_obj_t *)anim->var;
    if (obj) {
        lv_obj_del(obj);
    }
}

/* Start horizontal slide-in animation
 * from_right: true = slide from right to left, false = slide from left to right */
static void start_slide_in_x_anim(lv_obj_t *obj, bool from_right) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, layer_slide_x_anim_cb);
    int start_x = from_right ? 40 : -40;  /* Start 40px to the side */
    lv_anim_set_values(&anim, start_x, 0);  /* Animate to center (x_offset=0) */
    lv_anim_set_time(&anim, 150);  /* 150ms */
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

/* Start horizontal slide-out animation with auto-delete
 * to_left: true = slide to left, false = slide to right */
static void start_slide_out_x_anim(lv_obj_t *obj, bool to_left) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, layer_slide_x_anim_cb);
    int end_x = to_left ? -40 : 40;  /* End 40px to the side */
    lv_anim_set_values(&anim, 0, end_x);  /* From center to side */
    lv_anim_set_time(&anim, 80);  /* 80ms - fast disappear */
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&anim, slide_out_ready_cb);
    lv_anim_start(&anim);
}

/* Start pulse animation on active layer */
static void start_pulse_anim(lv_obj_t *obj) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, layer_pulse_anim_cb);
    lv_anim_set_values(&anim, 100, 125);  /* Scale 100% -> 125% */
    lv_anim_set_time(&anim, 100);  /* 100ms expand */
    lv_anim_set_playback_time(&anim, 100);  /* 100ms shrink back */
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

/* Slide in layer list labels from left */
static void start_layer_list_slide_in(void) {
    int num_layers = ds_max_layers;
    int spacing = 25;
    int label_width = 22;
    int start_x = 140 - ((num_layers - 1) * spacing / 2) - (label_width / 2);
    int slide_offset = 50;  /* Slide from 50px to the left */

    for (int i = 0; i < num_layers && i < 10 && layer_labels[i]; i++) {
        int target_x = start_x + (i * spacing);
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, layer_labels[i]);
        lv_anim_set_exec_cb(&anim, layer_pos_x_anim_cb);
        lv_anim_set_values(&anim, target_x - slide_offset, target_x);
        lv_anim_set_time(&anim, 150);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
        lv_anim_start(&anim);
    }
}

/* ========== Slide Mode Layer Display Functions ========== */

/* Slide mode layout with gradient fade:
 * 7 visible slots: [0] [1] [2] [3] [4] [5] [6]
 * - Slot 0,6: small (16pt, very dim) - fade out edges
 * - Slot 1-5: large (28pt, bright) - center zone (5 slots)
 *
 * Active layer stays in large zone (1-5), scrolls only when needed.
 * Negative window_start allowed - negative slots shown as empty.
 */

#define SLIDE_LARGE_ZONE_START 2  /* Index where large zone begins (slots 2,3,4,5,6) */
#define SLIDE_LARGE_ZONE_END 6    /* Index where large zone ends (inclusive) - 5 large slots */

/* Get font for slot based on gradient position:
 * Pattern: 小中大大大大大中小 (9 slots: 0-8)
 * Slot 0,8: Small (16pt) - edge
 * Slot 1,7: Medium (20pt) - transition
 * Slot 2,3,4,5,6: Large (28pt) - center (5 large slots)
 */
static const lv_font_t* get_slide_slot_font(int slot) {
    if (slot == 0 || slot == 8) {
        return &lv_font_montserrat_16;  /* Edge - small */
    } else if (slot == 1 || slot == 7) {
        return &lv_font_montserrat_20;  /* Transition - medium */
    }
    return &lv_font_montserrat_28;      /* Center - large (slots 2,3,4,5,6) */
}

/* Get opacity for slot based on gradient position (inactive):
 * Edge: very dim, Medium: dim, Large: visible
 */
static lv_opa_t get_slide_slot_opa(int slot) {
    if (slot == 0 || slot == 8) {
        return LV_OPA_20;  /* Edge - very dim (darker) */
    } else if (slot == 1 || slot == 7) {
        return LV_OPA_40;  /* Transition - dim (darker) */
    }
    return LV_OPA_70;      /* Center - visible */
}

/* Get Y adjustment for vertical alignment based on font size */
static int get_slide_slot_y_adj(int slot) {
    if (slot == 0 || slot == 8) {
        return 6;   /* Small font needs more Y adjustment */
    } else if (slot == 1 || slot == 7) {
        return 4;   /* Medium font needs some Y adjustment */
    }
    return 0;       /* Large font - no adjustment */
}

/* Get X offset to move edge slots slightly inward */
static int get_slide_slot_x_offset(int slot) {
    if (slot == 0) {
        return 4;   /* Move left edge slot inward (right) */
    } else if (slot == 8) {
        return -4;  /* Move right edge slot inward (left) */
    }
    return 0;
}

/* Slide mode layout constants - using fixed width labels like fixed mode */
#define SLIDE_SLOT_SPACING 34       /* Uniform spacing between slots (wider for 2-digit) */
#define SLIDE_LABEL_WIDTH_SMALL 22  /* Width for edge slots (small font) */
#define SLIDE_LABEL_WIDTH_MEDIUM 28 /* Width for transition slots (medium font) */
#define SLIDE_LABEL_WIDTH_LARGE 34  /* Width for center slots (large font) */

/* Get label width for slot based on font size */
static int get_slide_label_width(int slot) {
    if (slot == 0 || slot == 8) {
        return SLIDE_LABEL_WIDTH_SMALL;
    } else if (slot == 1 || slot == 7) {
        return SLIDE_LABEL_WIDTH_MEDIUM;
    }
    return SLIDE_LABEL_WIDTH_LARGE;
}

/* Create slide mode layer widgets */
static void create_layer_slide_widgets(lv_obj_t *parent, int y_offset) {
    /* Calculate window start to keep active_layer in large zone
     * Prefer left side of large zone so large zone stays visually centered */
    layer_slide_window_start = active_layer - SLIDE_LARGE_ZONE_START;
    /* DON'T clamp to 0 - allow negative values, empty slots for negative layers */

    /* Calculate start_x to center all slots */
    int total_width = (SLIDE_VISIBLE_COUNT - 1) * SLIDE_SLOT_SPACING;
    int start_x = 140 - (total_width / 2);

    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        int layer_num = layer_slide_window_start + i;
        bool is_active = (layer_num == active_layer && layer_num >= 0);
        int label_width = get_slide_label_width(i);

        layer_slide_labels[i] = lv_label_create(parent);

        /* Font size based on gradient position */
        lv_obj_set_style_text_font(layer_slide_labels[i], get_slide_slot_font(i), 0);

        /* Fixed width and center alignment for uniform spacing */
        lv_obj_set_width(layer_slide_labels[i], label_width);
        lv_obj_set_style_text_align(layer_slide_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(layer_slide_labels[i], LV_LABEL_LONG_CLIP);  /* Prevent text wrapping */

        /* Text color and opacity based on active state and gradient */
        if (layer_num < 0) {
            /* Negative layer number = empty/invisible */
            lv_obj_set_style_text_opa(layer_slide_labels[i], LV_OPA_TRANSP, 0);
        } else if (is_active) {
            /* Active layer = Hue-based color, full opacity */
            lv_obj_set_style_text_color(layer_slide_labels[i],
                get_slide_layer_color(layer_num, ds_layer_slide_max), 0);
            lv_obj_set_style_text_opa(layer_slide_labels[i], LV_OPA_COVER, 0);
        } else {
            /* Inactive = gray with gradient opacity */
            lv_obj_set_style_text_color(layer_slide_labels[i], lv_color_make(80, 80, 80), 0);
            lv_obj_set_style_text_opa(layer_slide_labels[i], get_slide_slot_opa(i), 0);
        }

        /* Set label text (layer number or empty for negative) */
        char text[8];
        if (layer_num >= 0) {
            snprintf(text, sizeof(text), "%d", layer_num);
        } else {
            text[0] = '\0';  /* Empty for negative */
        }
        lv_label_set_text(layer_slide_labels[i], text);

        /* Position using uniform spacing (center each label on its slot) */
        int y_adj = get_slide_slot_y_adj(i);
        int x_offset = get_slide_slot_x_offset(i);
        int x_pos = start_x + (i * SLIDE_SLOT_SPACING) - (label_width / 2) + x_offset;
        lv_obj_set_pos(layer_slide_labels[i], x_pos, y_offset + y_adj);

        /* Enable transform for animations */
        lv_obj_set_style_transform_pivot_x(layer_slide_labels[i], label_width / 2, 0);
        lv_obj_set_style_transform_pivot_y(layer_slide_labels[i], 14, 0);
    }

    LOG_INF("Slide mode widgets created: window_start=%d, active=%d", layer_slide_window_start, active_layer);
}

/* Destroy slide mode layer widgets */
static void destroy_layer_slide_widgets(void) {
    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        if (layer_slide_labels[i]) {
            lv_anim_del(layer_slide_labels[i], NULL);  /* Cancel any running animations */
            lv_obj_del(layer_slide_labels[i]);
            layer_slide_labels[i] = NULL;
        }
    }
    layer_slide_window_start = 0;
}

/* Reset all label positions to ensure they stay in correct place */
static void slide_reset_positions(void) {
    int total_width = (SLIDE_VISIBLE_COUNT - 1) * SLIDE_SLOT_SPACING;
    int start_x = 140 - (total_width / 2);

    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        if (layer_slide_labels[i]) {
            int label_width = get_slide_label_width(i);
            int y_adj = get_slide_slot_y_adj(i);
            int x_offset = get_slide_slot_x_offset(i);
            int x_pos = start_x + (i * SLIDE_SLOT_SPACING) - (label_width / 2) + x_offset;
            lv_obj_set_pos(layer_slide_labels[i], x_pos, 105 + y_adj);
        }
    }
}

/* Animation callback for scroll complete */
static void slide_scroll_anim_cb(void *var, int32_t value) {
    lv_obj_t *obj = (lv_obj_t *)var;
    if (obj) {
        lv_obj_set_style_translate_x(obj, value, 0);
    }
}

/* Update slide mode display - called when layer changes */
static void update_layer_slide_display(int layer, bool animate) {
    /* Auto-expand max layer if needed */
    if (layer >= ds_layer_slide_max) {
        ds_layer_slide_max = layer + 1;
        LOG_INF("Slide max expanded to %d", ds_layer_slide_max);
    }

    /* Calculate which slot the active layer is currently in */
    int current_slot = layer - layer_slide_window_start;

    /* Check if we need to scroll */
    bool need_scroll = false;
    int new_window_start = layer_slide_window_start;

    if (current_slot < SLIDE_LARGE_ZONE_START) {
        /* Layer moving left of large zone - scroll left */
        new_window_start = layer - SLIDE_LARGE_ZONE_START;
        need_scroll = true;
    } else if (current_slot > SLIDE_LARGE_ZONE_END) {
        /* Layer moving right of large zone - scroll right */
        new_window_start = layer - SLIDE_LARGE_ZONE_END;
        need_scroll = true;
    }

    /* DON'T clamp to 0 - allow negative values for edge layers (0, 1, 2) */

    /* Calculate scroll amount (number of slots shifted) */
    int scroll_slots = 0;
    if (need_scroll) {
        scroll_slots = new_window_start - layer_slide_window_start;
        layer_slide_window_start = new_window_start;
    }

    /* Update all labels with correct content and styling */
    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        if (!layer_slide_labels[i]) continue;

        int layer_num = layer_slide_window_start + i;
        bool is_active = (layer_num == layer && layer_num >= 0);

        /* Update text */
        char text[8];
        if (layer_num >= 0) {
            snprintf(text, sizeof(text), "%d", layer_num);
        } else {
            text[0] = '\0';  /* Empty for negative */
        }
        lv_label_set_text(layer_slide_labels[i], text);

        /* Update styling with gradient */
        if (layer_num < 0) {
            /* Negative layer = invisible */
            lv_obj_set_style_text_opa(layer_slide_labels[i], LV_OPA_TRANSP, 0);
        } else if (is_active) {
            /* Active layer = Hue-based color, full opacity */
            lv_obj_set_style_text_color(layer_slide_labels[i],
                get_slide_layer_color(layer_num, ds_layer_slide_max), 0);
            lv_obj_set_style_text_opa(layer_slide_labels[i], LV_OPA_COVER, 0);

            /* Pulse animation on active layer change */
            if (animate) {
                start_pulse_anim(layer_slide_labels[i]);
            }
        } else {
            /* Inactive = gray with gradient opacity based on slot position */
            lv_obj_set_style_text_color(layer_slide_labels[i], lv_color_make(80, 80, 80), 0);
            lv_obj_set_style_text_opa(layer_slide_labels[i], get_slide_slot_opa(i), 0);
        }
    }

    /* Set correct positions for all labels */
    slide_reset_positions();

    /* Apply scroll animation if scrolling occurred */
    if (need_scroll && animate && scroll_slots != 0) {
        /* Calculate scroll offset in pixels */
        int scroll_offset = scroll_slots * SLIDE_SLOT_SPACING;

        /* Animate all labels from offset position to final position */
        for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
            if (!layer_slide_labels[i]) continue;

            /* Cancel any existing translate animation */
            lv_anim_del(layer_slide_labels[i], slide_scroll_anim_cb);

            /* Start from offset position and animate to 0 (final position) */
            lv_anim_t anim;
            lv_anim_init(&anim);
            lv_anim_set_var(&anim, layer_slide_labels[i]);
            lv_anim_set_exec_cb(&anim, slide_scroll_anim_cb);
            lv_anim_set_values(&anim, scroll_offset, 0);
            lv_anim_set_time(&anim, 150);
            lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
            lv_anim_start(&anim);
        }
    }

    LOG_DBG("Slide update: layer=%d, window_start=%d, slot=%d, scroll=%d",
            layer, layer_slide_window_start, layer - layer_slide_window_start, scroll_slots);
}

void display_update_layer(int layer) {
    if (layer < 0 || layer > 255) return;

    int prev_layer = active_layer;
    active_layer = layer;  /* Always cache the value */

    /* Only update UI when on main screen */
    if (current_screen != SCREEN_MAIN) {
        return;
    }

    /* ========== Slide Mode ========== */
    if (ds_layer_slide_mode) {
        /* Slide mode: just update the slide display, it handles everything */
        bool animate = (prev_layer != layer);
        update_layer_slide_display(layer, animate);
        last_active_layer = layer;
        return;
    }

    /* ========== Fixed Mode (original behavior) ========== */
    bool should_be_over_max = (layer >= ds_max_layers);
    int layer_y = 105;  /* Y position for layer widgets */

    /* Determine slide direction based on layer change */
    bool going_up = (layer > prev_layer);  /* Layer increasing = slide from right */

    /* Mode transition: normal -> over-max */
    if (should_be_over_max && !layer_mode_over_max) {
        layer_mode_over_max = true;

        /* Slide-out layer list to left (animation callback will delete) */
        for (int i = 0; i < 10; i++) {
            if (layer_labels[i]) {
                start_slide_out_x_anim(layer_labels[i], true);  /* to_left = true */
                layer_labels[i] = NULL;
            }
        }

        /* Create over-max widget with slide-in from right */
        if (screen_obj) {
            create_over_max_widget(screen_obj, layer, layer_y);
            start_slide_in_x_anim(layer_over_max_label, true);  /* from_right = true */
        }
    }
    /* Mode transition: over-max -> normal */
    else if (!should_be_over_max && layer_mode_over_max) {
        layer_mode_over_max = false;

        /* Slide-out over-max widget to right (animation callback will delete) */
        if (layer_over_max_label) {
            start_slide_out_x_anim(layer_over_max_label, false);  /* to_left = false */
            layer_over_max_label = NULL;
        }

        /* Create layer list with slide-in from left */
        if (screen_obj) {
            create_layer_list_widgets(screen_obj, layer_y);
            start_layer_list_slide_in();
        }
    }
    /* Update within over-max mode */
    else if (layer_mode_over_max) {
        if (prev_layer != layer && screen_obj) {
            /* Slide out old number, slide in new number */
            if (layer_over_max_label) {
                /* Slide out: going_up = slide to left, going_down = slide to right */
                start_slide_out_x_anim(layer_over_max_label, going_up);
                layer_over_max_label = NULL;
            }

            /* Create new label and slide in from opposite direction */
            create_over_max_widget(screen_obj, layer, layer_y);
            start_slide_in_x_anim(layer_over_max_label, going_up);  /* from_right if going_up */
        }
    }
    else {
        /* Update normal layer list - just update colors, pulse on active */
        for (int i = 0; i < ds_max_layers && i < 10 && layer_labels[i]; i++) {
            if (i == active_layer) {
                lv_obj_set_style_text_color(layer_labels[i], get_layer_color(i), 0);
                lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_COVER, 0);

                /* Pulse animation on active layer change */
                if (prev_layer != layer) {
                    start_pulse_anim(layer_labels[i]);
                }
            } else {
                lv_obj_set_style_text_color(layer_labels[i], lv_color_make(40, 40, 40), 0);
                lv_obj_set_style_text_opa(layer_labels[i], LV_OPA_30, 0);
            }
        }
    }

    last_active_layer = layer;
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

/* Helper function to reposition battery widgets based on count */
static void reposition_battery_widgets(int count) {
    if (count < 1) count = 1;
    if (count > MAX_KB_BATTERIES) count = MAX_KB_BATTERIES;

    /* Select layout based on count */
    static const int16_t kb_x_offsets_1[] = {0, 0, 0, 0};
    static const int16_t kb_x_offsets_2[] = {-70, 70, 0, 0};
    static const int16_t kb_x_offsets_3[] = {-90, 0, 90, 0};
    static const int16_t kb_x_offsets_4[] = {-100, -35, 35, 100};

    const int16_t *x_offsets;
    int16_t bar_width;

    switch (count) {
    case 1:
        x_offsets = kb_x_offsets_1;
        bar_width = 165;  /* 1.5x of standard width */
        break;
    case 2:
        x_offsets = kb_x_offsets_2;
        bar_width = 110;
        break;
    case 3:
        x_offsets = kb_x_offsets_3;
        bar_width = 70;
        break;
    case 4:
    default:
        x_offsets = kb_x_offsets_4;
        bar_width = 52;
        break;
    }

    /* Get name labels based on count */
    const char **names = NULL;
    if (count == 2) names = battery_names_2;
    else if (count == 3) names = battery_names_3;
    else if (count == 4) names = battery_names_4;

    #define KB_BAR_Y_OFFSET_R    -33
    #define KB_PCT_Y_OFFSET_R    -42
    #define KB_NAME_X_OFFSET_R   0

    for (int i = 0; i < MAX_KB_BATTERIES; i++) {
        bool visible = (i < count);
        int16_t x_off = x_offsets[i];

        /* Reposition bar */
        if (kb_bat_bar[i]) {
            lv_obj_set_size(kb_bat_bar[i], bar_width, 4);
            lv_obj_align(kb_bat_bar[i], LV_ALIGN_BOTTOM_MID, x_off, KB_BAR_Y_OFFSET_R);
        }

        /* Reposition percentage label */
        if (kb_bat_pct[i]) {
            lv_obj_align(kb_bat_pct[i], LV_ALIGN_BOTTOM_MID, x_off, KB_PCT_Y_OFFSET_R);
        }

        /* Reposition and set name label */
        if (kb_bat_name[i]) {
            lv_obj_align(kb_bat_name[i], LV_ALIGN_BOTTOM_MID, x_off - bar_width/2 + KB_NAME_X_OFFSET_R, KB_PCT_Y_OFFSET_R);
            if (visible && names && names[i]) {
                lv_label_set_text(kb_bat_name[i], names[i]);
            } else {
                lv_label_set_text(kb_bat_name[i], "");
            }
        }

        /* Reposition nc bar */
        if (kb_bat_nc_bar[i]) {
            lv_obj_set_size(kb_bat_nc_bar[i], bar_width, 4);
            lv_obj_align(kb_bat_nc_bar[i], LV_ALIGN_BOTTOM_MID, x_off, KB_BAR_Y_OFFSET_R);
            /* Hide unused slots, but keep visible slots ready (visibility controlled by update function) */
            if (!visible) {
                lv_obj_set_style_opa(kb_bat_nc_bar[i], 0, 0);
            }
            lv_obj_invalidate(kb_bat_nc_bar[i]);  /* Force redraw */
        }

        /* Reposition nc label */
        if (kb_bat_nc_label[i]) {
            lv_obj_align(kb_bat_nc_label[i], LV_ALIGN_BOTTOM_MID, x_off, KB_PCT_Y_OFFSET_R);
            /* Hide unused slots */
            if (!visible) {
                lv_obj_set_style_opa(kb_bat_nc_label[i], 0, 0);
            }
            lv_obj_invalidate(kb_bat_nc_label[i]);  /* Force redraw */
        }

        /* Force redraw for bar, pct, name */
        if (kb_bat_bar[i]) lv_obj_invalidate(kb_bat_bar[i]);
        if (kb_bat_pct[i]) lv_obj_invalidate(kb_bat_pct[i]);
        if (kb_bat_name[i]) lv_obj_invalidate(kb_bat_name[i]);
    }

    LOG_INF("Battery widgets repositioned for count=%d", count);
}

void display_update_keyboard_battery_4(int bat0, int bat1, int bat2, int bat3) {
    int values[MAX_KB_BATTERIES] = {bat0, bat1, bat2, bat3};

    /* Count active batteries */
    int count = 0;
    for (int i = 0; i < MAX_KB_BATTERIES; i++) {
        battery_values[i] = values[i];
        if (values[i] > 0) count++;
    }

    /* If count changed, reposition widgets (including count=0 case) */
    if (count != active_battery_count) {
        active_battery_count = count;
        if (count > 0) {
            reposition_battery_widgets(count);
        }
        /* When count=0, hide all widgets below */
    }

    /* Update each battery slot */
    for (int i = 0; i < MAX_KB_BATTERIES; i++) {
        bool slot_visible = (count > 0 && i < count);
        int val = values[i];

        if (slot_visible && val > 0) {
            /* Connected: show bar and percentage, hide × */
            if (kb_bat_nc_bar[i]) lv_obj_set_style_opa(kb_bat_nc_bar[i], 0, 0);
            if (kb_bat_nc_label[i]) lv_obj_set_style_opa(kb_bat_nc_label[i], 0, 0);
            if (kb_bat_bar[i]) {
                lv_obj_set_style_opa(kb_bat_bar[i], 255, LV_PART_MAIN);
                lv_obj_set_style_opa(kb_bat_bar[i], 255, LV_PART_INDICATOR);
                lv_bar_set_value(kb_bat_bar[i], val, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(kb_bat_bar[i], get_keyboard_battery_color(val), LV_PART_INDICATOR);
            }
            if (kb_bat_pct[i]) {
                lv_obj_set_style_opa(kb_bat_pct[i], 255, 0);
                char buf[16];
                snprintf(buf, sizeof(buf), "%d", val);
                lv_label_set_text(kb_bat_pct[i], buf);
                lv_obj_set_style_text_color(kb_bat_pct[i], get_keyboard_battery_color(val), 0);
            }
            if (kb_bat_name[i]) {
                lv_obj_set_style_opa(kb_bat_name[i], 255, 0);
            }
        } else if (slot_visible) {
            /* Disconnected: show ×, hide bar and percentage */
            if (kb_bat_bar[i]) {
                lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_MAIN);
                lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_INDICATOR);
            }
            if (kb_bat_pct[i]) lv_obj_set_style_opa(kb_bat_pct[i], 0, 0);
            if (kb_bat_name[i]) lv_obj_set_style_opa(kb_bat_name[i], 255, 0);
            if (kb_bat_nc_bar[i]) lv_obj_set_style_opa(kb_bat_nc_bar[i], 255, 0);
            if (kb_bat_nc_label[i]) lv_obj_set_style_opa(kb_bat_nc_label[i], 255, 0);
        } else {
            /* Slot not visible (beyond active count or count=0) - hide everything */
            if (kb_bat_bar[i]) {
                lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_MAIN);
                lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_INDICATOR);
            }
            if (kb_bat_pct[i]) lv_obj_set_style_opa(kb_bat_pct[i], 0, 0);
            if (kb_bat_name[i]) lv_obj_set_style_opa(kb_bat_name[i], 0, 0);
            if (kb_bat_nc_bar[i]) lv_obj_set_style_opa(kb_bat_nc_bar[i], 0, 0);
            if (kb_bat_nc_label[i]) lv_obj_set_style_opa(kb_bat_nc_label[i], 0, 0);
        }
    }
}

/* Legacy 2-battery interface for backward compatibility */
void display_update_keyboard_battery(int left, int right) {
    display_update_keyboard_battery_4(left, right, 0, 0);
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
        /* Robust rate display: handle invalid/out-of-range values */
        if (rate < 0.0f) {
            /* Negative = no data yet */
            snprintf(buf, sizeof(buf), "-.--Hz");
        } else if (rate > 999.9f || rate != rate) {  /* rate != rate checks for NaN */
            LOG_WRN("Invalid rate value: %.2f, displaying as -.--", (double)rate);
            snprintf(buf, sizeof(buf), "-.--Hz");
        } else {
            /* Safe conversion with rounding */
            int rate_int = (int)(rate * 10.0f + 0.5f);
            if (rate_int > 9999) rate_int = 9999;  /* Cap at 999.9Hz */
            snprintf(buf, sizeof(buf), "%d.%dHz", rate_int / 10, rate_int % 10);
        }
        lv_label_set_text(rate_label, buf);
    }
}

/* ========== Screen Transition Functions ========== */

static void destroy_main_screen_widgets(void) {
    LOG_INF("Destroying main screen widgets...");

    /* Cancel any running layer animations BEFORE deleting objects */
    for (int i = 0; i < 10; i++) {
        if (layer_labels[i]) {
            lv_anim_del(layer_labels[i], NULL);  /* Cancel all animations on this object */
        }
    }
    if (layer_over_max_label) {
        lv_anim_del(layer_over_max_label, NULL);
    }
    /* Cancel slide mode animations */
    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        if (layer_slide_labels[i]) {
            lv_anim_del(layer_slide_labels[i], NULL);
        }
    }

    if (rate_label) { lv_obj_del(rate_label); rate_label = NULL; }
    if (rssi_label) { lv_obj_del(rssi_label); rssi_label = NULL; }
    if (rssi_bar) { lv_obj_del(rssi_bar); rssi_bar = NULL; }
    if (rx_title_label) { lv_obj_del(rx_title_label); rx_title_label = NULL; }
    if (channel_label) { lv_obj_del(channel_label); channel_label = NULL; }
    /* Keyboard battery - delete all 4 slots */
    for (int i = 0; i < MAX_KB_BATTERIES; i++) {
        if (kb_bat_nc_label[i]) { lv_obj_del(kb_bat_nc_label[i]); kb_bat_nc_label[i] = NULL; }
        if (kb_bat_nc_bar[i]) { lv_obj_del(kb_bat_nc_bar[i]); kb_bat_nc_bar[i] = NULL; }
        if (kb_bat_name[i]) { lv_obj_del(kb_bat_name[i]); kb_bat_name[i] = NULL; }
        if (kb_bat_pct[i]) { lv_obj_del(kb_bat_pct[i]); kb_bat_pct[i] = NULL; }
        if (kb_bat_bar[i]) { lv_obj_del(kb_bat_bar[i]); kb_bat_bar[i] = NULL; }
    }
    if (modifier_label) { lv_obj_del(modifier_label); modifier_label = NULL; }
    for (int i = 0; i < 10; i++) {
        if (layer_labels[i]) { lv_obj_del(layer_labels[i]); layer_labels[i] = NULL; }
    }
    if (layer_over_max_label) { lv_obj_del(layer_over_max_label); layer_over_max_label = NULL; }
    /* Delete slide mode widgets */
    for (int i = 0; i < SLIDE_VISIBLE_COUNT; i++) {
        if (layer_slide_labels[i]) { lv_obj_del(layer_slide_labels[i]); layer_slide_labels[i] = NULL; }
    }
    if (layer_title_label) { lv_obj_del(layer_title_label); layer_title_label = NULL; }
    if (ble_profile_label) { lv_obj_del(ble_profile_label); ble_profile_label = NULL; }
    if (transport_label) { lv_obj_del(transport_label); transport_label = NULL; }
    if (wpm_value_label) { lv_obj_del(wpm_value_label); wpm_value_label = NULL; }
    if (wpm_title_label) { lv_obj_del(wpm_title_label); wpm_title_label = NULL; }
    if (scanner_bat_pct) { lv_obj_del(scanner_bat_pct); scanner_bat_pct = NULL; }
    if (scanner_bat_icon) { lv_obj_del(scanner_bat_icon); scanner_bat_icon = NULL; }
    if (device_name_label) { lv_obj_del(device_name_label); device_name_label = NULL; }

    /* Reset state for proper reinitialization */
    layer_mode_over_max = false;
    active_battery_count = 0;  /* Force reposition on next update */

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

    /* Hide battery widget if disabled */
    if (!ds_battery_visible) {
        lv_obj_set_style_opa(scanner_bat_icon, 0, 0);
        lv_obj_set_style_opa(scanner_bat_pct, 0, 0);
    }

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

    /* Create layer display - slide mode OR fixed mode (list/over-max) */
    if (ds_layer_slide_mode) {
        /* Slide mode: create 7-slot dial display */
        create_layer_slide_widgets(screen_obj, 105);
        layer_mode_over_max = false;  /* Not used in slide mode */
    } else if (active_layer >= ds_max_layers) {
        layer_mode_over_max = true;
        create_over_max_widget(screen_obj, active_layer, 105);
    } else {
        layer_mode_over_max = false;
        create_layer_list_widgets(screen_obj, 105);
    }

    modifier_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(modifier_label, &NerdFonts_Regular_40, 0);
    lv_obj_set_style_text_color(modifier_label, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(modifier_label, 10, 0);  /* Space between icons */
    lv_label_set_text(modifier_label, "");
    lv_obj_align(modifier_label, LV_ALIGN_TOP_MID, 0, 145);

    /* === Keyboard battery widgets (4 slots, dynamic layout) === */
    static const int16_t kb_x_offsets_2_r[] = {-70, 70, 0, 0};
    int16_t bar_width_r = 110;  /* Default to 2-battery layout */

    for (int i = 0; i < MAX_KB_BATTERIES; i++) {
        int16_t x_offset_r = (i < 2) ? kb_x_offsets_2_r[i] : 0;

        /* Connected state bar */
        kb_bat_bar[i] = lv_bar_create(screen_obj);
        lv_obj_set_size(kb_bat_bar[i], bar_width_r, 4);
        lv_obj_align(kb_bat_bar[i], LV_ALIGN_BOTTOM_MID, x_offset_r, -33);
        lv_bar_set_range(kb_bat_bar[i], 0, 100);
        lv_bar_set_value(kb_bat_bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(kb_bat_bar[i], lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(kb_bat_bar[i], 255, LV_PART_MAIN);
        lv_obj_set_style_radius(kb_bat_bar[i], 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(kb_bat_bar[i], lv_color_hex(0x909090), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(kb_bat_bar[i], 255, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(kb_bat_bar[i], lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(kb_bat_bar[i], LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
        lv_obj_set_style_radius(kb_bat_bar[i], 1, LV_PART_INDICATOR);
        lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_MAIN);
        lv_obj_set_style_opa(kb_bat_bar[i], 0, LV_PART_INDICATOR);

        /* Percentage label */
        kb_bat_pct[i] = lv_label_create(screen_obj);
        lv_obj_set_style_text_font(kb_bat_pct[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(kb_bat_pct[i], lv_color_white(), 0);
        lv_obj_align(kb_bat_pct[i], LV_ALIGN_BOTTOM_MID, x_offset_r, -42);
        lv_label_set_text(kb_bat_pct[i], "0");
        lv_obj_set_style_opa(kb_bat_pct[i], 0, 0);

        /* Name label */
        kb_bat_name[i] = lv_label_create(screen_obj);
        lv_obj_set_style_text_font(kb_bat_name[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(kb_bat_name[i], lv_color_hex(0x808080), 0);
        lv_obj_align(kb_bat_name[i], LV_ALIGN_BOTTOM_MID, x_offset_r - bar_width_r/2, -42);
        lv_obj_set_style_text_align(kb_bat_name[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(kb_bat_name[i], "");
        lv_obj_set_style_opa(kb_bat_name[i], 0, 0);

        /* Disconnected state bar */
        kb_bat_nc_bar[i] = lv_obj_create(screen_obj);
        lv_obj_set_size(kb_bat_nc_bar[i], bar_width_r, 4);
        lv_obj_align(kb_bat_nc_bar[i], LV_ALIGN_BOTTOM_MID, x_offset_r, -33);
        lv_obj_set_style_bg_color(kb_bat_nc_bar[i], lv_color_hex(0x9e2121), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(kb_bat_nc_bar[i], 255, LV_PART_MAIN);
        lv_obj_set_style_radius(kb_bat_nc_bar[i], 1, LV_PART_MAIN);
        lv_obj_set_style_border_width(kb_bat_nc_bar[i], 0, 0);
        lv_obj_set_style_pad_all(kb_bat_nc_bar[i], 0, 0);
        lv_obj_set_style_opa(kb_bat_nc_bar[i], (i < 2) ? 255 : 0, 0);

        /* Disconnected state label */
        kb_bat_nc_label[i] = lv_label_create(screen_obj);
        lv_obj_set_style_text_font(kb_bat_nc_label[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(kb_bat_nc_label[i], lv_color_hex(0xe63030), 0);
        lv_obj_align(kb_bat_nc_label[i], LV_ALIGN_BOTTOM_MID, x_offset_r, -42);
        lv_label_set_text(kb_bat_nc_label[i], LV_SYMBOL_CLOSE);
        lv_obj_set_style_opa(kb_bat_nc_label[i], (i < 2) ? 255 : 0, 0);
    }

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
    lv_label_set_text(rate_label, "-.--Hz");
    lv_obj_set_pos(rate_label, 222, 219);  /* 5px down, 5px left */

    LOG_INF("Main screen widgets created, restoring cached values...");

    /* Restore all cached values to newly created widgets */
    display_update_device_name(cached_device_name);
    display_update_scanner_battery(scanner_battery);
    display_update_wpm(wpm_value);
    display_update_connection(usb_ready, ble_connected, ble_bonded, ble_profile);
    display_update_layer(active_layer);
    display_update_modifiers(cached_modifiers);

    /* Force battery widget reposition based on cached values */
    /* Count how many batteries have non-zero values */
    int cached_count = 0;
    for (int i = 0; i < MAX_KB_BATTERIES; i++) {
        if (battery_values[i] > 0) cached_count++;
    }
    if (cached_count > 0) {
        /* Force reposition with cached count */
        active_battery_count = cached_count;
        reposition_battery_widgets(cached_count);
        LOG_INF("Battery widgets repositioned for cached count=%d", cached_count);
    }
    /* Now update battery display with values */
    display_update_keyboard_battery_4(battery_values[0], battery_values[1], battery_values[2], battery_values[3]);

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

        /* Real-time label and value update while dragging */
        if (slider == ds_brightness_slider && ds_brightness_value) {
            /* Update brightness label */
            lv_label_set_text_fmt(ds_brightness_value, "%d%%", (int)new_value);
            /* Apply brightness in real-time for immediate visual feedback */
            set_pwm_brightness((uint8_t)new_value);
        } else if (slider == ds_layer_slider && ds_layer_value) {
            /* Update layer count label */
            lv_label_set_text_fmt(ds_layer_value, "%d", (int)new_value);
        }

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

/* Slide mode switch handler */
static void ds_slide_switch_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;

    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ds_layer_slide_mode = checked;

    LOG_INF("Layer slide mode: %s", checked ? "ON" : "OFF");

    /* Rebuild layer display on main screen if we have screen_obj */
    /* This will be handled when returning to main screen */
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
    if (ds_slide_switch) { lv_obj_del(ds_slide_switch); ds_slide_switch = NULL; }
    if (ds_slide_label) { lv_obj_del(ds_slide_label); ds_slide_label = NULL; }
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

    y_pos += 30;  /* Compact spacing */

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

    y_pos += 35;  /* Slightly more space before Max Layers */

    /* ===== Max Layers Section ===== */
    ds_layer_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_layer_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds_layer_label, lv_color_white(), 0);
    lv_label_set_text(ds_layer_label, "Max Layers");
    lv_obj_set_pos(ds_layer_label, 15, y_pos);

    /* Slide label and switch (same row as Max Layers, like Auto is on Brightness row) */
    ds_slide_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_slide_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ds_slide_label, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(ds_slide_label, "Slide");
    lv_obj_set_pos(ds_slide_label, 195, y_pos + 4);  /* Same x as Auto label */

    ds_slide_switch = lv_switch_create(screen_obj);
    lv_obj_set_size(ds_slide_switch, 50, 28);  /* Same size as Auto switch */
    lv_obj_set_pos(ds_slide_switch, 230, y_pos);  /* Same x as Auto switch */
    if (ds_layer_slide_mode) {
        lv_obj_add_state(ds_slide_switch, LV_STATE_CHECKED);
    }
    /* iOS-style switch styling (same as Auto) */
    lv_obj_set_style_radius(ds_slide_switch, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ds_slide_switch, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ds_slide_switch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ds_slide_switch, 14, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ds_slide_switch, lv_color_hex(0x34C759), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(ds_slide_switch, lv_color_hex(0x3A3A3C), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ds_slide_switch, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(ds_slide_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_color(ds_slide_switch, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(ds_slide_switch, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ds_slide_switch, -2, LV_PART_KNOB);
    lv_obj_set_style_border_width(ds_slide_switch, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(ds_slide_switch, 15);  /* Same as Auto */
    lv_obj_add_event_cb(ds_slide_switch, ds_slide_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    y_pos += 35;  /* Space between Max Layers label and slider */

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

    /* Layer value label (aligned with Brightness value at x=230) */
    ds_layer_value = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ds_layer_value, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds_layer_value, lv_color_hex(0x007AFF), 0);
    snprintf(buf, sizeof(buf), "%d", ds_max_layers);
    lv_label_set_text(ds_layer_value, buf);
    lv_obj_set_pos(ds_layer_value, 230, y_pos);

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
    if (ss_version_label) { lv_obj_del(ss_version_label); ss_version_label = NULL; }
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

    /* Version label - gray, small, centered between title and bootloader button */
    ss_version_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ss_version_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ss_version_label, lv_color_hex(0x808080), 0);
    lv_label_set_text(ss_version_label, "Prospector Scanner v2.1b");
    lv_obj_align(ss_version_label, LV_ALIGN_TOP_MID, 0, 52);

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

/* Forward declaration for badge tap callback */
static void ks_badge_tap_cb(lv_event_t *e);

/* Create a single keyboard entry at absolute position */
static void ks_create_entry(int entry_idx, int y_pos, int keyboard_index,
                            const char *name, int8_t rssi, uint8_t channel) {
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

    /* Channel badge (only shown when scanner channel is "All") */
    entry->channel_badge = lv_obj_create(entry->container);
    lv_obj_set_size(entry->channel_badge, 20, 18);
    lv_obj_align(entry->channel_badge, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(entry->channel_badge, get_channel_color(channel), 0);
    lv_obj_set_style_bg_opa(entry->channel_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(entry->channel_badge, 4, 0);
    lv_obj_set_style_border_width(entry->channel_badge, 0, 0);
    lv_obj_set_style_pad_all(entry->channel_badge, 0, 0);

    lv_obj_t *ch_label = lv_label_create(entry->channel_badge);
    char ch_buf[4];
    snprintf(ch_buf, sizeof(ch_buf), "%d", channel);
    lv_label_set_text(ch_label, ch_buf);
    lv_obj_set_style_text_color(ch_label, lv_color_hex(0x000000), 0);  /* Dark text */
    lv_obj_set_style_text_font(ch_label, &lv_font_montserrat_12, 0);
    lv_obj_center(ch_label);

    /* Make badge clickable to filter by this channel */
    lv_obj_add_flag(entry->channel_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(entry->channel_badge, ks_badge_tap_cb,
                       LV_EVENT_CLICKED, (void *)(intptr_t)channel);

    /* Always show channel badge (including ch0 = broadcast) */
    /* Offset to make room for badge */
    int left_offset = 30;

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
    lv_obj_align(entry->rssi_bar, LV_ALIGN_LEFT_MID, left_offset, 0);

    /* RSSI label */
    entry->rssi_label = lv_label_create(entry->container);
    char rssi_buf[16];
    snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", rssi);
    lv_label_set_text(entry->rssi_label, rssi_buf);
    lv_obj_set_style_text_color(entry->rssi_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_font(entry->rssi_label, &lv_font_montserrat_12, 0);
    lv_obj_align(entry->rssi_label, LV_ALIGN_LEFT_MID, left_offset + 34, 0);

    /* Keyboard name */
    entry->name_label = lv_label_create(entry->container);
    lv_label_set_text(entry->name_label, name);
    lv_obj_set_style_text_color(entry->name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(entry->name_label, &lv_font_montserrat_16, 0);
    lv_obj_align(entry->name_label, LV_ALIGN_LEFT_MID, left_offset + 92, 0);

    LOG_DBG("Created keyboard entry %d: %s (rssi=%d, ch=%d)", entry_idx, name, rssi, channel);
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
    entry->channel_badge = NULL;
    entry->keyboard_index = -1;
}

/* ========== Channel Selector Helpers ========== */

/*
 * Channel values:
 *   0-9  = Specific channel filter (only show keyboards on that channel)
 *   10   = "All" - show all keyboards regardless of channel
 *
 * This design handles backwards compatibility:
 *   - Old keyboards broadcast on ch0 by default
 *   - When scanner is set to ch1-9, old keyboards (ch0) are filtered out
 *   - When scanner is set to "All" (10), everything is shown
 */
#define CHANNEL_ALL 10
#define CHANNEL_MAX 10  /* 0-9 = specific channels, 10 = All */

/* Forward declaration */
static void ks_update_entries(void);

static void ks_update_channel_display(void) {
    if (!ks_channel_value || !ks_channel_container) return;

    uint8_t ch = scanner_get_runtime_channel();
    if (ch == CHANNEL_ALL) {
        lv_label_set_text(ks_channel_value, "All");
        lv_obj_set_style_bg_color(ks_channel_container, lv_color_hex(0x4A90E2), LV_STATE_DEFAULT);  /* Blue for All */
        lv_obj_set_style_text_color(ks_channel_value, lv_color_hex(0x000000), 0);    /* Dark text */
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", ch);
        lv_label_set_text(ks_channel_value, buf);
        lv_obj_set_style_bg_color(ks_channel_container, get_channel_color(ch), LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(ks_channel_value, lv_color_hex(0x000000), 0);    /* Dark text */
    }
}

/* Change channel and update UI */
static void ks_channel_change(uint8_t new_channel) {
    if (new_channel > CHANNEL_MAX) new_channel = 0;
    scanner_set_runtime_channel(new_channel);
    ks_update_channel_display();
    ks_update_entries();  /* Refresh list immediately */
    LOG_INF("Channel changed to %d", new_channel);
}

/* Increment/decrement channel (for swipe gestures) */
static void ks_channel_increment(void) {
    uint8_t ch = scanner_get_runtime_channel();
    ch = (ch < CHANNEL_MAX) ? ch + 1 : 0;
    ks_channel_change(ch);
}

static void ks_channel_decrement(void) {
    uint8_t ch = scanner_get_runtime_channel();
    ch = (ch > 0) ? ch - 1 : CHANNEL_MAX;
    ks_channel_change(ch);
}

/* Close channel popup */
static void ks_close_channel_popup(void) {
    if (ks_channel_popup) {
        lv_obj_del(ks_channel_popup);
        ks_channel_popup = NULL;
        for (int i = 0; i <= CHANNEL_MAX; i++) {
            ks_channel_popup_btns[i] = NULL;
        }
    }
}

/* Channel popup button callback */
static void ks_channel_popup_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    intptr_t channel = (intptr_t)lv_event_get_user_data(e);
    ks_close_channel_popup();
    ks_channel_change((uint8_t)channel);
}

/* Show channel popup */
static void ks_show_channel_popup(void) {
    if (ks_channel_popup) {
        ks_close_channel_popup();
        return;  /* Toggle off if already showing */
    }

    /* Create popup background (semi-transparent overlay) */
    ks_channel_popup = lv_obj_create(screen_obj);
    lv_obj_set_size(ks_channel_popup, 210, 200);  /* 4 rows of badges + title, centered */
    lv_obj_align(ks_channel_popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ks_channel_popup, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(ks_channel_popup, LV_OPA_90, 0);
    lv_obj_set_style_radius(ks_channel_popup, 12, 0);
    lv_obj_set_style_border_color(ks_channel_popup, lv_color_hex(0x404040), 0);
    lv_obj_set_style_border_width(ks_channel_popup, 2, 0);
    lv_obj_set_style_pad_all(ks_channel_popup, 10, 0);
    lv_obj_clear_flag(ks_channel_popup, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(ks_channel_popup);
    lv_label_set_text(title, "Channel Select");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    uint8_t current_ch = scanner_get_runtime_channel();

    /*
     * Badge-style layout:
     *   Row 0: [  All  ] [0]
     *   Row 1: [1] [2] [3]
     *   Row 2: [4] [5] [6]
     *   Row 3: [7] [8] [9]
     */
    #define BADGE_W 48
    #define BADGE_H 28
    #define BADGE_GAP_X 6
    #define BADGE_GAP_Y 6
    #define BADGE_START_Y 35

    /* Calculate centering for 3-column layout (popup 210px wide, padding 10 each side = 190 inner) */
    int total_width = 3 * BADGE_W + 2 * BADGE_GAP_X;  /* 156px */
    int start_x = (210 - 20 - total_width) / 2;  /* (190 - 156) / 2 = 17 */

    /* Row 0: "All" (2 col width) + "0" (1 col) */
    int all_width = 2 * BADGE_W + BADGE_GAP_X;

    ks_channel_popup_btns[CHANNEL_ALL] = lv_btn_create(ks_channel_popup);
    lv_obj_set_size(ks_channel_popup_btns[CHANNEL_ALL], all_width, BADGE_H);
    lv_obj_align(ks_channel_popup_btns[CHANNEL_ALL], LV_ALIGN_TOP_LEFT, start_x, BADGE_START_Y);
    lv_obj_set_style_bg_color(ks_channel_popup_btns[CHANNEL_ALL], lv_color_hex(0x4A90E2), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ks_channel_popup_btns[CHANNEL_ALL], LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ks_channel_popup_btns[CHANNEL_ALL], 6, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ks_channel_popup_btns[CHANNEL_ALL], 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ks_channel_popup_btns[CHANNEL_ALL], 0, LV_STATE_DEFAULT);
    if (current_ch == CHANNEL_ALL) {
        lv_obj_set_style_border_color(ks_channel_popup_btns[CHANNEL_ALL], lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ks_channel_popup_btns[CHANNEL_ALL], 2, LV_STATE_DEFAULT);
    }
    lv_obj_t *all_lbl = lv_label_create(ks_channel_popup_btns[CHANNEL_ALL]);
    lv_label_set_text(all_lbl, "All");
    lv_obj_set_style_text_color(all_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(all_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(all_lbl);
    lv_obj_add_event_cb(ks_channel_popup_btns[CHANNEL_ALL], ks_channel_popup_btn_cb,
                       LV_EVENT_CLICKED, (void *)(intptr_t)CHANNEL_ALL);

    /* "0" badge on same row as "All" */
    ks_channel_popup_btns[0] = lv_btn_create(ks_channel_popup);
    lv_obj_set_size(ks_channel_popup_btns[0], BADGE_W, BADGE_H);
    lv_obj_align(ks_channel_popup_btns[0], LV_ALIGN_TOP_LEFT,
                 start_x + all_width + BADGE_GAP_X, BADGE_START_Y);
    lv_obj_set_style_bg_color(ks_channel_popup_btns[0], get_channel_color(0), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ks_channel_popup_btns[0], LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ks_channel_popup_btns[0], 6, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ks_channel_popup_btns[0], 0, LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(ks_channel_popup_btns[0], 0, LV_STATE_DEFAULT);
    if (current_ch == 0) {
        lv_obj_set_style_border_color(ks_channel_popup_btns[0], lv_color_white(), LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ks_channel_popup_btns[0], 2, LV_STATE_DEFAULT);
    }
    lv_obj_t *lbl0 = lv_label_create(ks_channel_popup_btns[0]);
    lv_label_set_text(lbl0, "0");
    lv_obj_set_style_text_color(lbl0, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl0, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl0);
    lv_obj_add_event_cb(ks_channel_popup_btns[0], ks_channel_popup_btn_cb,
                       LV_EVENT_CLICKED, (void *)(intptr_t)0);

    /* Channels 1-9 in 3-column grid (rows 1-3) */
    for (int i = 1; i <= 9; i++) {
        int idx = i - 1;  /* 0-based index for grid positioning */
        int row = idx / 3;
        int col = idx % 3;
        int y_offset = BADGE_START_Y + BADGE_H + BADGE_GAP_Y + row * (BADGE_H + BADGE_GAP_Y);
        int x_offset = start_x + col * (BADGE_W + BADGE_GAP_X);

        ks_channel_popup_btns[i] = lv_btn_create(ks_channel_popup);
        lv_obj_set_size(ks_channel_popup_btns[i], BADGE_W, BADGE_H);
        lv_obj_align(ks_channel_popup_btns[i], LV_ALIGN_TOP_LEFT, x_offset, y_offset);

        lv_obj_set_style_bg_color(ks_channel_popup_btns[i], get_channel_color(i), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ks_channel_popup_btns[i], LV_OPA_COVER, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(ks_channel_popup_btns[i], 6, LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(ks_channel_popup_btns[i], 0, LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(ks_channel_popup_btns[i], 0, LV_STATE_DEFAULT);

        if (i == current_ch) {
            lv_obj_set_style_border_color(ks_channel_popup_btns[i], lv_color_white(), LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(ks_channel_popup_btns[i], 2, LV_STATE_DEFAULT);
        }

        lv_obj_t *lbl = lv_label_create(ks_channel_popup_btns[i]);
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", i);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(ks_channel_popup_btns[i], ks_channel_popup_btn_cb,
                           LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    #undef BADGE_W
    #undef BADGE_H
    #undef BADGE_GAP_X
    #undef BADGE_GAP_Y
    #undef BADGE_START_Y
}

/* Callback when channel display is tapped */
static void ks_channel_display_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ks_show_channel_popup();
}

/* Callback when a keyboard's channel badge is tapped */
static void ks_badge_tap_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    intptr_t channel = (intptr_t)lv_event_get_user_data(e);
    ks_channel_change((uint8_t)channel);
}

/* Update keyboard entries with current scanner data */
static void ks_update_entries(void) {
    /* Count active keyboards (with channel filtering) */
    int active_keyboards[KS_MAX_KEYBOARDS];
    int active_count = 0;

    uint8_t scanner_ch = scanner_get_runtime_channel();

    for (int i = 0; i < CONFIG_PROSPECTOR_MAX_KEYBOARDS && active_count < KS_MAX_KEYBOARDS; i++) {
        struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
        if (!kbd || !kbd->active) continue;

        /* Channel filtering:
         *   scanner_ch = CHANNEL_ALL (10): Show all keyboards
         *   scanner_ch = 0-9: Only show keyboards with matching channel
         */
        if (scanner_ch != CHANNEL_ALL && kbd->data.channel != scanner_ch) {
            continue;  /* Skip keyboards that don't match filter */
        }

        active_keyboards[active_count++] = i;
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
        int y_pos = 55;  /* Start below title/channel selector */
        int spacing = 40;

        for (int i = 0; i < active_count; i++) {
            int kbd_idx = active_keyboards[i];
            struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(kbd_idx);
            if (!kbd) continue;

            const char *name = kbd->ble_name[0] ? kbd->ble_name : "Unknown";
            uint8_t channel = kbd->data.channel;  /* Get keyboard's channel */
            ks_create_entry(i, y_pos + (i * spacing), kbd_idx, name, kbd->rssi, channel);
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

    /* Close popup if open */
    ks_close_channel_popup();

    /* Destroy channel selector widgets */
    if (ks_channel_container) { lv_obj_del(ks_channel_container); ks_channel_container = NULL; }
    ks_channel_value = NULL;  /* Deleted with container */

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

    /* Title (left side) */
    ks_title_label = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ks_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ks_title_label, lv_color_white(), 0);
    lv_label_set_text(ks_title_label, "Keyboards");
    lv_obj_align(ks_title_label, LV_ALIGN_TOP_LEFT, 15, 15);

    /* ========== Channel Badge (right side of header) ========== */
    /* "Ch:" label + colored badge - tappable */

    /* "Ch:" prefix label - clickable */
    lv_obj_t *ch_prefix = lv_label_create(screen_obj);
    lv_obj_set_style_text_font(ch_prefix, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ch_prefix, lv_color_hex(0x808080), 0);
    lv_label_set_text(ch_prefix, "Ch:");
    lv_obj_align(ch_prefix, LV_ALIGN_TOP_RIGHT, -55, 19);
    lv_obj_add_flag(ch_prefix, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ch_prefix, ks_channel_display_cb, LV_EVENT_CLICKED, NULL);

    /* Channel badge - use simple btn for reliable click */
    ks_channel_container = lv_btn_create(screen_obj);
    lv_obj_set_size(ks_channel_container, 36, 24);
    lv_obj_align(ks_channel_container, LV_ALIGN_TOP_RIGHT, -15, 16);
    lv_obj_set_style_radius(ks_channel_container, 6, 0);
    lv_obj_set_style_pad_all(ks_channel_container, 0, 0);
    lv_obj_set_style_shadow_width(ks_channel_container, 0, 0);
    lv_obj_set_style_bg_opa(ks_channel_container, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ks_channel_container, ks_channel_display_cb, LV_EVENT_CLICKED, NULL);

    /* Channel value label (centered in badge) - also clickable, triggers same callback */
    ks_channel_value = lv_label_create(ks_channel_container);
    lv_obj_set_style_text_font(ks_channel_value, &lv_font_montserrat_12, 0);
    lv_obj_center(ks_channel_value);
    lv_obj_add_flag(ks_channel_value, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ks_channel_value, ks_channel_display_cb, LV_EVENT_CLICKED, NULL);
    ks_update_channel_display();

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

/* ========== Pong Wars Screen ========== */

/*
 * Pong Wars - Pre-allocated cell version (stable)
 * - ALL cell objects created at init (no dynamic alloc during game)
 * - Cell conversion just changes color (no create/delete)
 * - Much more stable on resource-constrained devices
 */

#define PW_CELL_SIZE 20       /* Cell size in pixels */
#define PW_GRID_W 12          /* 240 / 20 = 12 cells */
#define PW_GRID_H 9           /* 180 / 20 = 9 cells */
#define PW_NUM_CELLS (PW_GRID_W * PW_GRID_H)  /* 108 cells total */
#define PW_NUM_BALLS 2
#define PW_BALL_RADIUS 6
#define PW_ARENA_W 240        /* 80% of 280 */
#define PW_ARENA_H 180        /* 75% of 240 */
#define PW_OFFSET_X 20        /* (280 - 240) / 2 */
#define PW_OFFSET_Y 30        /* Top margin for score */

/* Pastel color palettes - {team1_bg, team2_bg, team1_ball, team2_ball} */
static const uint32_t pw_color_palettes[][4] = {
    {0xFFB5E8, 0xB5DEFF, 0xFF4D6D, 0x2D8CFF},  /* Pink vs Blue */
    {0xFFDEB5, 0xB5FFD9, 0xFF8C42, 0x2ECC71},  /* Orange vs Green */
    {0xE8B5FF, 0xFFFDB5, 0x9B59B6, 0xF1C40F},  /* Purple vs Yellow */
    {0xB5FFE8, 0xFFB5C5, 0x1ABC9C, 0xE74C3C},  /* Cyan vs Red */
    {0xD5B5FF, 0xB5F0FF, 0x8E44AD, 0x3498DB},  /* Violet vs Sky */
    {0xFFE5B5, 0xC5FFB5, 0xE67E22, 0x27AE60},  /* Peach vs Lime */
};
#define PW_NUM_PALETTES (sizeof(pw_color_palettes) / sizeof(pw_color_palettes[0]))

/* Current colors (set randomly at start) */
static uint32_t pw_color_team1 = 0xFFB5E8;
static uint32_t pw_color_team2 = 0xB5DEFF;
static uint32_t pw_color_ball1 = 0xFF4D6D;
static uint32_t pw_color_ball2 = 0x2D8CFF;

/* State */
static lv_timer_t *pw_timer = NULL;
static uint8_t pw_grid[PW_NUM_CELLS];  /* Cell ownership: 0=team1, 1=team2 */
static lv_obj_t *pw_cell_objs[PW_NUM_CELLS];  /* Pre-allocated cell objects */
static lv_obj_t *pw_arena_container = NULL;  /* Container for arena */
static lv_obj_t *pw_ball_objs[PW_NUM_BALLS];
static lv_obj_t *pw_score_label1 = NULL;  /* Team1 score */
static lv_obj_t *pw_score_label2 = NULL;  /* Team2 score */
static bool pw_initialized = false;
static uint32_t pw_rand_seed = 12345;
static int16_t pw_base_speed = 25;  /* Random base speed */

/* Ball state - pixel coordinates with fixed-point velocity */
struct pw_ball {
    int16_t x, y;      /* Pixel position */
    int16_t dx, dy;    /* Velocity (pixels * 10 for precision) */
    uint8_t team;
};
static struct pw_ball pw_balls[PW_NUM_BALLS];
static int pw_score1 = 0, pw_score2 = 0;

/* Forward declarations */
static void pw_tap_handler(lv_event_t *e);
static void pw_reset_game(void);

/* Simple random number generator */
static uint32_t pw_rand(void) {
    pw_rand_seed = pw_rand_seed * 1103515245 + 12345;
    return (pw_rand_seed >> 16) & 0x7FFF;
}

/* Initialize grid data and update cell colors */
static void pw_init_grid(void) {
    pw_score1 = 0;
    pw_score2 = 0;
    for (int y = 0; y < PW_GRID_H; y++) {
        for (int x = 0; x < PW_GRID_W; x++) {
            int idx = y * PW_GRID_W + x;
            pw_grid[idx] = (x < PW_GRID_W / 2) ? 0 : 1;
            if (pw_grid[idx] == 0) pw_score1++; else pw_score2++;

            /* Update pre-allocated cell color */
            if (pw_cell_objs[idx]) {
                uint32_t color = (pw_grid[idx] == 0) ? pw_color_team1 : pw_color_team2;
                lv_obj_set_style_bg_color(pw_cell_objs[idx], lv_color_hex(color), 0);
            }
        }
    }
}

static void pw_select_random_palette(void) {
    int idx = pw_rand() % PW_NUM_PALETTES;
    pw_color_team1 = pw_color_palettes[idx][0];
    pw_color_team2 = pw_color_palettes[idx][1];
    pw_color_ball1 = pw_color_palettes[idx][2];
    pw_color_ball2 = pw_color_palettes[idx][3];
    LOG_INF("Pong Wars palette: %d", idx);
}

static void pw_init_balls(void) {
    /* Use uptime as random seed */
    pw_rand_seed = k_uptime_get_32();

    /* Random color palette */
    pw_select_random_palette();

    /* Random base speed (40-60) - faster gameplay */
    pw_base_speed = 40 + (pw_rand() % 21);
    LOG_INF("Pong Wars speed: %d", pw_base_speed);

    for (int i = 0; i < PW_NUM_BALLS; i++) {
        pw_balls[i].team = i;  /* Ball 0 = team 0, Ball 1 = team 1 */

        /* Start position: team 0 on left, team 1 on right */
        if (i == 0) {
            pw_balls[i].x = PW_ARENA_W / 4;
            pw_balls[i].y = PW_ARENA_H / 2;
        } else {
            pw_balls[i].x = PW_ARENA_W * 3 / 4;
            pw_balls[i].y = PW_ARENA_H / 2;
        }

        /* Random velocity with random base speed */
        int angle_idx = pw_rand() % 8;
        /* Vary velocity slightly around base_speed */
        int vx = pw_base_speed + (pw_rand() % 10) - 5;
        int vy = pw_base_speed + (pw_rand() % 10) - 5;

        /* Direction based on angle index */
        int sx = (angle_idx < 4) ? 1 : -1;
        int sy = ((angle_idx % 4) < 2) ? 1 : -1;

        pw_balls[i].dx = vx * sx;
        pw_balls[i].dy = vy * sy;

        /* Team 0 goes right, Team 1 goes left initially */
        if (i == 0 && pw_balls[i].dx < 0) pw_balls[i].dx = -pw_balls[i].dx;
        if (i == 1 && pw_balls[i].dx > 0) pw_balls[i].dx = -pw_balls[i].dx;
    }
}

/* Update cell color (no object creation/deletion!) */
static void pw_update_cell_color(int gx, int gy, uint8_t team) {
    int idx = gy * PW_GRID_W + gx;
    if (idx < 0 || idx >= PW_NUM_CELLS) return;
    if (!pw_cell_objs[idx]) return;

    uint32_t color = (team == 0) ? pw_color_team1 : pw_color_team2;
    lv_obj_set_style_bg_color(pw_cell_objs[idx], lv_color_hex(color), 0);
}

static void pw_update_ball_display(int i) {
    if (!pw_ball_objs[i]) return;
    lv_obj_set_pos(pw_ball_objs[i], pw_balls[i].x - PW_BALL_RADIUS, pw_balls[i].y - PW_BALL_RADIUS);
}

static void pw_update_score(void) {
    char buf[8];
    if (pw_score_label1) {
        snprintf(buf, sizeof(buf), "%d", pw_score1);
        lv_label_set_text(pw_score_label1, buf);
    }
    if (pw_score_label2) {
        snprintf(buf, sizeof(buf), "%d", pw_score2);
        lv_label_set_text(pw_score_label2, buf);
    }
}

static void pw_step(void) {
    if (!pw_initialized) return;

    for (int i = 0; i < PW_NUM_BALLS; i++) {
        struct pw_ball *b = &pw_balls[i];

        /* Move ball (velocity is *10, so divide by 10) */
        int new_x = b->x + b->dx / 10;
        int new_y = b->y + b->dy / 10;

        /* Bounce off walls */
        if (new_x < PW_BALL_RADIUS) {
            new_x = PW_BALL_RADIUS;
            b->dx = -b->dx;
        } else if (new_x > PW_ARENA_W - PW_BALL_RADIUS) {
            new_x = PW_ARENA_W - PW_BALL_RADIUS;
            b->dx = -b->dx;
        }
        if (new_y < PW_BALL_RADIUS) {
            new_y = PW_BALL_RADIUS;
            b->dy = -b->dy;
        } else if (new_y > PW_ARENA_H - PW_BALL_RADIUS) {
            new_y = PW_ARENA_H - PW_BALL_RADIUS;
            b->dy = -b->dy;
        }

        /* Check which grid cell we're in */
        int gx = new_x / PW_CELL_SIZE;
        int gy = new_y / PW_CELL_SIZE;
        if (gx >= 0 && gx < PW_GRID_W && gy >= 0 && gy < PW_GRID_H) {
            int idx = gy * PW_GRID_W + gx;
            if (pw_grid[idx] != b->team) {
                /* Convert cell! (just update color, no object creation) */
                pw_grid[idx] = b->team;
                pw_update_cell_color(gx, gy, b->team);
                if (b->team == 0) { pw_score1++; pw_score2--; }
                else { pw_score2++; pw_score1--; }

                /* Bounce - vary the angle slightly */
                int r = pw_rand() % 3;
                if (r == 0) b->dx = -b->dx;
                else if (r == 1) b->dy = -b->dy;
                else { b->dx = -b->dx; b->dy = -b->dy; }
            }
        }

        b->x = new_x;
        b->y = new_y;
        pw_update_ball_display(i);
    }
}

static void pw_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    if (!pw_initialized) return;

    /* Frame counter */
    static uint16_t frame_count = 0;
    frame_count++;

    /* Run physics (no object creation, just color updates) */
    pw_step();

    /* Update score every ~10 frames */
    if (frame_count % 10 == 0) {
        pw_update_score();
    }
}

static void destroy_pong_wars_widgets(void) {
    /* CRITICAL: Resume background display updates */
    pong_wars_active = false;

    pw_initialized = false;
    if (pw_timer) { lv_timer_del(pw_timer); pw_timer = NULL; }
    /* Cell objects will be cleaned by lv_obj_clean(screen_obj) in swipe handler */
    memset(pw_cell_objs, 0, sizeof(pw_cell_objs));
    memset(pw_ball_objs, 0, sizeof(pw_ball_objs));
    pw_arena_container = NULL;
    pw_score_label1 = NULL;
    pw_score_label2 = NULL;
    LOG_INF("Pong Wars destroyed");
}

static void pw_tap_handler(lv_event_t *e) {
    ARG_UNUSED(e);
    LOG_INF("Pong Wars: tap detected, resetting...");
    pw_reset_game();
}

static void create_pong_wars_widgets(void) {
    LOG_INF("Creating Pong Wars (smooth version)...");

    pw_initialized = false;
    pw_init_grid();
    pw_init_balls();  /* This sets random colors and speed */

    /* Dark background for whole screen */
    lv_obj_set_style_bg_color(screen_obj, lv_color_hex(0x1a1a2e), 0);

    /* Title "Pong Wars" centered at top (white text) */
    lv_obj_t *title = lv_label_create(screen_obj);
    if (title) {
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_label_set_text(title, "Pong Wars");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);
    }

    /* Score labels - aligned with arena edges */
    /* Arena: x=PW_OFFSET_X(20) to x=PW_OFFSET_X+PW_ARENA_W(260) */
    pw_score_label1 = lv_label_create(screen_obj);
    if (pw_score_label1) {
        lv_obj_set_style_text_font(pw_score_label1, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(pw_score_label1, lv_color_hex(pw_color_ball1), 0);
        lv_obj_set_style_bg_color(pw_score_label1, lv_color_hex(pw_color_team1), 0);
        lv_obj_set_style_bg_opa(pw_score_label1, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pw_score_label1, 8, 0);
        lv_obj_set_style_pad_ver(pw_score_label1, 2, 0);
        lv_obj_set_style_radius(pw_score_label1, 6, 0);
        lv_obj_set_pos(pw_score_label1, PW_OFFSET_X, 6);  /* Left edge of arena */
    }

    pw_score_label2 = lv_label_create(screen_obj);
    if (pw_score_label2) {
        lv_obj_set_style_text_font(pw_score_label2, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(pw_score_label2, lv_color_hex(pw_color_ball2), 0);
        lv_obj_set_style_bg_color(pw_score_label2, lv_color_hex(pw_color_team2), 0);
        lv_obj_set_style_bg_opa(pw_score_label2, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(pw_score_label2, 8, 0);
        lv_obj_set_style_pad_ver(pw_score_label2, 2, 0);
        lv_obj_set_style_radius(pw_score_label2, 6, 0);
        /* Right-align to arena right edge (arena ends at x=260, label ~30px wide) */
        lv_obj_set_pos(pw_score_label2, PW_OFFSET_X + PW_ARENA_W - 35, 6);
    }

    /* Arena container with rounded border */
    pw_arena_container = lv_obj_create(screen_obj);
    if (pw_arena_container) {
        lv_obj_remove_style_all(pw_arena_container);
        lv_obj_set_size(pw_arena_container, PW_ARENA_W, PW_ARENA_H);
        lv_obj_set_pos(pw_arena_container, PW_OFFSET_X, PW_OFFSET_Y);
        lv_obj_set_style_radius(pw_arena_container, 8, 0);
        lv_obj_set_style_clip_corner(pw_arena_container, true, 0);
        lv_obj_set_style_border_color(pw_arena_container, lv_color_hex(0x404060), 0);
        lv_obj_set_style_border_width(pw_arena_container, 2, 0);
        lv_obj_clear_flag(pw_arena_container, LV_OBJ_FLAG_SCROLLABLE);
        /* Add tap handler for reset */
        lv_obj_add_flag(pw_arena_container, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pw_arena_container, pw_tap_handler, LV_EVENT_CLICKED, NULL);
    }

    /* Pre-allocate ALL cell objects (no dynamic alloc during game!) */
    for (int y = 0; y < PW_GRID_H; y++) {
        for (int x = 0; x < PW_GRID_W; x++) {
            int idx = y * PW_GRID_W + x;
            lv_obj_t *cell = lv_obj_create(pw_arena_container);
            if (!cell) continue;

            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, PW_CELL_SIZE, PW_CELL_SIZE);
            lv_obj_set_pos(cell, x * PW_CELL_SIZE, y * PW_CELL_SIZE);

            /* Initial color based on grid position */
            uint32_t color = (x < PW_GRID_W / 2) ? pw_color_team1 : pw_color_team2;
            lv_obj_set_style_bg_color(cell, lv_color_hex(color), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            pw_cell_objs[idx] = cell;
        }
    }
    LOG_INF("Pre-allocated %d cell objects", PW_NUM_CELLS);

    /* Create balls inside arena (using dynamic colors) */
    lv_color_t ball_colors[2] = {lv_color_hex(pw_color_ball1), lv_color_hex(pw_color_ball2)};
    for (int i = 0; i < PW_NUM_BALLS; i++) {
        lv_obj_t *ball = lv_obj_create(pw_arena_container);
        if (!ball) continue;
        lv_obj_remove_style_all(ball);
        lv_obj_set_size(ball, PW_BALL_RADIUS * 2, PW_BALL_RADIUS * 2);
        lv_obj_set_style_bg_color(ball, ball_colors[pw_balls[i].team], 0);
        lv_obj_set_style_bg_opa(ball, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(ball, lv_color_white(), 0);
        lv_obj_set_style_border_width(ball, 2, 0);
        lv_obj_set_style_shadow_color(ball, lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_width(ball, 4, 0);
        lv_obj_set_style_shadow_opa(ball, LV_OPA_50, 0);
        lv_obj_clear_flag(ball, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        pw_ball_objs[i] = ball;
        pw_update_ball_display(i);
    }

    pw_update_score();

    /* Nav hint at bottom */
    lv_obj_t *hint = lv_label_create(screen_obj);
    if (hint) {
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x606080), 0);
        lv_label_set_text(hint, LV_SYMBOL_RIGHT " swipe to return");
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    pw_initialized = true;

    /* CRITICAL: Stop background display updates to prevent thread conflicts */
    pong_wars_active = true;

    /* High framerate for smooth animation (30 FPS) */
    pw_timer = lv_timer_create(pw_timer_cb, 33, NULL);  /* 30 FPS */
    LOG_INF("Pong Wars started! (smooth mode, background updates paused)");
}

/* Reset game with new random colors and speed (pre-allocated cells version) */
static void pw_reset_game(void) {
    if (!pw_initialized) return;
    if (!pw_arena_container) return;

    /* Stop timer during reset */
    pw_initialized = false;
    if (pw_timer) {
        lv_timer_del(pw_timer);
        pw_timer = NULL;
    }

    /* Reinitialize grid and balls with new random colors/speed */
    pw_init_grid();
    pw_init_balls();

    /* Update ALL pre-allocated cell colors to match new grid state */
    for (int y = 0; y < PW_GRID_H; y++) {
        for (int x = 0; x < PW_GRID_W; x++) {
            int idx = y * PW_GRID_W + x;
            if (pw_cell_objs[idx]) {
                uint32_t color = (pw_grid[idx] == 0) ? pw_color_team1 : pw_color_team2;
                lv_obj_set_style_bg_color(pw_cell_objs[idx], lv_color_hex(color), 0);
            }
        }
    }

    /* Update ball colors and positions (balls already exist) */
    lv_color_t ball_colors[2] = {lv_color_hex(pw_color_ball1), lv_color_hex(pw_color_ball2)};
    for (int i = 0; i < PW_NUM_BALLS; i++) {
        if (pw_ball_objs[i]) {
            lv_obj_set_style_bg_color(pw_ball_objs[i], ball_colors[pw_balls[i].team], 0);
            /* Move balls to foreground */
            lv_obj_move_foreground(pw_ball_objs[i]);
            pw_update_ball_display(i);
        }
    }

    /* Update score labels with new colors */
    if (pw_score_label1) {
        lv_obj_set_style_text_color(pw_score_label1, lv_color_hex(pw_color_ball1), 0);
        lv_obj_set_style_bg_color(pw_score_label1, lv_color_hex(pw_color_team1), 0);
    }
    if (pw_score_label2) {
        lv_obj_set_style_text_color(pw_score_label2, lv_color_hex(pw_color_ball2), 0);
        lv_obj_set_style_bg_color(pw_score_label2, lv_color_hex(pw_color_team2), 0);
    }

    pw_update_score();

    /* Restart timer */
    pw_initialized = true;
    pw_timer = lv_timer_create(pw_timer_cb, 33, NULL);  /* 30 FPS */
    LOG_INF("Pong Wars reset! (new colors/speed)");
}

/* ========== Swipe Processing (runs in LVGL timer = Main Thread) ========== */

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
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
#else
/* No-op when touch is disabled */
static inline void ensure_lvgl_indev_registered(void) {}
#endif

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
        /* Main → Pong Wars OR Quick Actions → Main */
        if (current_screen == SCREEN_MAIN) {
            LOG_INF(">>> Transitioning: MAIN -> PONG_WARS");
            destroy_main_screen_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
            lv_obj_invalidate(screen_obj);
            create_pong_wars_widgets();
            ensure_lvgl_indev_registered();  /* Register for tap-to-reset */
            current_screen = SCREEN_PONG_WARS;
            LOG_INF(">>> Transition complete");
        } else if (current_screen == SCREEN_SYSTEM_SETTINGS) {
            LOG_INF(">>> Transitioning: QUICK_ACTIONS -> MAIN");
            destroy_system_settings_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
            lv_obj_invalidate(screen_obj);
            create_main_screen_widgets();
            current_screen = SCREEN_MAIN;
            LOG_INF(">>> Transition complete");
        } else if (current_screen == SCREEN_KEYBOARD_SELECT) {
            /* Channel decrement on left swipe */
            ks_close_channel_popup();  /* Close popup if open */
            ks_channel_decrement();
            LOG_INF(">>> Keyboard Select: Channel decremented");
        }
        break;

    case SWIPE_DIRECTION_RIGHT:
        /* Pong Wars → Main OR Main → Quick Actions */
        if (current_screen == SCREEN_PONG_WARS) {
            LOG_INF(">>> Transitioning: PONG_WARS -> MAIN");
            destroy_pong_wars_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_black(), 0);
            lv_obj_invalidate(screen_obj);
            create_main_screen_widgets();
            current_screen = SCREEN_MAIN;
            LOG_INF(">>> Transition complete");
        } else if (current_screen == SCREEN_MAIN) {
            LOG_INF(">>> Transitioning: MAIN -> QUICK_ACTIONS");
            destroy_main_screen_widgets();
            lv_obj_clean(screen_obj);
            lv_obj_set_style_bg_color(screen_obj, lv_color_hex(0x0A0A0A), 0);
            lv_obj_invalidate(screen_obj);
            create_system_settings_widgets();
            ensure_lvgl_indev_registered();  /* Register for button touch */
            current_screen = SCREEN_SYSTEM_SETTINGS;
            LOG_INF(">>> Transition complete");
        } else if (current_screen == SCREEN_KEYBOARD_SELECT) {
            /* Channel increment on right swipe */
            ks_close_channel_popup();  /* Close popup if open */
            ks_channel_increment();
            LOG_INF(">>> Keyboard Select: Channel incremented");
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
