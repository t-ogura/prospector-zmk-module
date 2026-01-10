/**
 * Scanner Message Handler - Connects BLE scanner to display widgets
 *
 * Receives keyboard advertisement data from status_scanner.c and
 * stores it for the display to render.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zmk/status_advertisement.h>
#include <lvgl.h>

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

LOG_MODULE_REGISTER(scanner_handler, LOG_LEVEL_INF);

/* External scanner start function (from status_scanner.c) */
extern int zmk_status_scanner_start(void);

/* Message queue definition (required by status_scanner.c) */
K_MSGQ_DEFINE(scanner_msgq, sizeof(struct { uint8_t dummy[128]; }), 32, 4);

/* Statistics */
static uint32_t msgs_sent = 0;
static uint32_t msgs_dropped = 0;
static uint32_t msgs_processed = 0;

/* Display update work (deferred to main thread) */
static void display_update_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(display_update_work, display_update_work_handler);
static volatile bool display_update_pending = false;

/* ========== Keyboard Data Storage ========== */

#define MAX_KEYBOARDS 3
#define MAX_NAME_LEN 32

struct keyboard_state {
    bool active;
    struct zmk_status_adv_data data;
    int8_t rssi;
    char name[MAX_NAME_LEN];
    uint32_t last_seen;  // k_uptime_get_32()
    uint8_t ble_addr[6];     // BLE MAC address for unique identification
    uint8_t ble_addr_type;   // BLE address type
};

static struct keyboard_state keyboards[MAX_KEYBOARDS];
static int selected_keyboard = 0;
static struct k_mutex data_mutex;
static bool mutex_initialized = false;

/* ========== External Widget Update Functions ========== */
/* These are implemented in custom_status_screen.c */

extern void display_update_device_name(const char *name);
extern void display_update_layer(int layer);
extern void display_update_wpm(int wpm);
extern void display_update_connection(bool usb_ready, bool ble_connected, bool ble_bonded, int profile);
extern void display_update_modifiers(uint8_t mods);
extern void display_update_keyboard_battery(int left, int right);
extern void display_update_signal(int8_t rssi, float rate_hz);
extern void display_update_scanner_battery(int level);

/* ========== Public API for Display ========== */

bool scanner_get_keyboard_data(int index, struct zmk_status_adv_data *data,
                               int8_t *rssi, char *name, size_t name_len) {
    if (!mutex_initialized || index < 0 || index >= MAX_KEYBOARDS) {
        return false;
    }

    if (k_mutex_lock(&data_mutex, K_MSEC(10)) != 0) {
        return false;
    }

    bool result = false;
    if (keyboards[index].active) {
        if (data) *data = keyboards[index].data;
        if (rssi) *rssi = keyboards[index].rssi;
        if (name && name_len > 0) {
            strncpy(name, keyboards[index].name, name_len - 1);
            name[name_len - 1] = '\0';
        }
        result = true;
    }

    k_mutex_unlock(&data_mutex);
    return result;
}

int scanner_get_active_keyboard_count(void) {
    if (!mutex_initialized) return 0;

    if (k_mutex_lock(&data_mutex, K_MSEC(10)) != 0) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) count++;
    }

    k_mutex_unlock(&data_mutex);
    return count;
}

int scanner_get_selected_keyboard(void) {
    return selected_keyboard;
}

/* Forward declaration */
static void schedule_display_update(void);

void scanner_set_selected_keyboard(int index) {
    if (index >= 0 && index < MAX_KEYBOARDS) {
        selected_keyboard = index;
        LOG_INF("Selected keyboard changed to slot %d", index);
        /* Immediately update display with new keyboard data */
        schedule_display_update();
    }
}

/* ========== Display Update Work (runs in system work queue context) ========== */

/* Rate calculation state */
static uint32_t rate_last_update = 0;
static int rate_update_count = 0;
static int8_t last_rssi = -100;

/* External flags from custom_status_screen.c */
extern volatile bool transition_in_progress;
extern volatile bool pong_wars_active;

/* Scanner battery update interval */
static uint32_t scanner_battery_last_update = 0;
#define SCANNER_BATTERY_UPDATE_INTERVAL_MS 5000  /* Update every 5 seconds */

static void display_update_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    display_update_pending = false;

    /* Skip ALL updates when Pong Wars is active (different LVGL screen) */
    if (pong_wars_active) {
        return;
    }

    /* Skip update if screen transition in progress */
    if (transition_in_progress) {
        LOG_DBG("Skipping update - transition in progress");
        return;
    }

    /* Update scanner's own battery periodically */
    uint32_t now = k_uptime_get_32();
    if (scanner_battery_last_update == 0 ||
        (now - scanner_battery_last_update) >= SCANNER_BATTERY_UPDATE_INTERVAL_MS) {
        scanner_msg_send_battery_update();
        scanner_battery_last_update = now;
    }

    struct zmk_status_adv_data data;
    int8_t rssi;
    char name[MAX_NAME_LEN];

    if (!scanner_get_keyboard_data(selected_keyboard, &data, &rssi, name, sizeof(name))) {
        LOG_DBG("No keyboard data for slot %d", selected_keyboard);
        return;
    }

    LOG_INF("Updating display: %s, Layer=%d, Battery=%d%%",
            name, data.active_layer, data.battery_level);

    /* Update all widgets with the keyboard data */
    display_update_device_name(name);
    display_update_layer(data.active_layer);
    display_update_wpm(data.wpm_value);

    bool usb_ready = (data.status_flags & ZMK_STATUS_FLAG_USB_HID_READY) != 0;
    bool ble_connected = (data.status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED) != 0;
    bool ble_bonded = (data.status_flags & ZMK_STATUS_FLAG_BLE_BONDED) != 0;
    display_update_connection(usb_ready, ble_connected, ble_bonded, data.profile_slot);

    display_update_modifiers(data.modifier_flags);

    /* For split keyboards, peripheral_battery[0] = left, [1] = right */
    int left_bat = data.peripheral_battery[0];
    int right_bat = data.peripheral_battery[1];
    /* If no peripheral batteries, use main battery */
    if (left_bat == 0 && right_bat == 0) {
        left_bat = data.battery_level;
        right_bat = data.battery_level;
    }
    display_update_keyboard_battery(left_bat, right_bat);

    /* Calculate reception rate */
    last_rssi = rssi;
    rate_update_count++;

    if (now - rate_last_update >= 1000) {
        float rate = (float)rate_update_count * 1000.0f / (float)(now - rate_last_update);
        display_update_signal(rssi, rate);
        rate_update_count = 0;
        rate_last_update = now;
    }
}

static void schedule_display_update(void) {
    /* Skip scheduling entirely when Pong Wars is active */
    if (pong_wars_active) {
        return;
    }

    if (!display_update_pending) {
        display_update_pending = true;
        /* Schedule with small delay to batch rapid updates */
        k_work_schedule(&display_update_work, K_MSEC(50));
    }
}

/* ========== Scanner Message Functions ========== */

int scanner_msg_send_keyboard_data(const struct zmk_status_adv_data *adv_data,
                                   int8_t rssi, const char *device_name,
                                   const uint8_t *ble_addr, uint8_t ble_addr_type) {
    if (!mutex_initialized) {
        k_mutex_init(&data_mutex);
        mutex_initialized = true;
    }

    if (k_mutex_lock(&data_mutex, K_MSEC(5)) != 0) {
        msgs_dropped++;
        return -EBUSY;
    }

    /* Find existing keyboard or empty slot */
    int index = -1;
    uint32_t keyboard_id = (adv_data->keyboard_id[0] << 24) |
                           (adv_data->keyboard_id[1] << 16) |
                           (adv_data->keyboard_id[2] << 8) |
                           adv_data->keyboard_id[3];

    /* PRIORITY: Look for existing keyboard by BLE address (unique per device)
     * This fixes the same-name keyboard conflict issue */
    if (ble_addr != NULL) {
        for (int i = 0; i < MAX_KEYBOARDS; i++) {
            if (keyboards[i].active) {
                if (memcmp(keyboards[i].ble_addr, ble_addr, 6) == 0) {
                    index = i;
                    break;
                }
            }
        }
    }

    /* Fallback: look for existing keyboard with same ID (for backward compatibility) */
    if (index < 0) {
        for (int i = 0; i < MAX_KEYBOARDS; i++) {
            if (keyboards[i].active) {
                uint32_t stored_id = (keyboards[i].data.keyboard_id[0] << 24) |
                                     (keyboards[i].data.keyboard_id[1] << 16) |
                                     (keyboards[i].data.keyboard_id[2] << 8) |
                                     keyboards[i].data.keyboard_id[3];
                if (stored_id == keyboard_id) {
                    index = i;
                    break;
                }
            }
        }
    }

    /* If not found, find empty slot */
    if (index < 0) {
        for (int i = 0; i < MAX_KEYBOARDS; i++) {
            if (!keyboards[i].active) {
                index = i;
                if (ble_addr) {
                    LOG_INF("New keyboard in slot %d: %s (BLE=%02X:%02X:%02X:%02X:%02X:%02X)",
                           index, device_name ? device_name : "(null)",
                           ble_addr[5], ble_addr[4], ble_addr[3],
                           ble_addr[2], ble_addr[1], ble_addr[0]);
                } else {
                    LOG_INF("New keyboard in slot %d: %s (ID=%08X)",
                           index, device_name ? device_name : "(null)", keyboard_id);
                }
                break;
            }
        }
    }

    if (index < 0) {
        k_mutex_unlock(&data_mutex);
        LOG_WRN("No slot for keyboard ID=%08X", keyboard_id);
        msgs_dropped++;
        return -ENOMEM;
    }

    /* Store the data */
    keyboards[index].active = true;
    memcpy(&keyboards[index].data, adv_data, sizeof(struct zmk_status_adv_data));
    keyboards[index].rssi = rssi;
    keyboards[index].last_seen = k_uptime_get_32();

    /* Store BLE address for unique identification */
    if (ble_addr) {
        memcpy(keyboards[index].ble_addr, ble_addr, 6);
        keyboards[index].ble_addr_type = ble_addr_type;
    }

    if (device_name && device_name[0] != '\0') {
        strncpy(keyboards[index].name, device_name, MAX_NAME_LEN - 1);
        keyboards[index].name[MAX_NAME_LEN - 1] = '\0';
    } else if (keyboards[index].name[0] == '\0') {
        snprintf(keyboards[index].name, MAX_NAME_LEN, "Keyboard %d", index);
    }

    k_mutex_unlock(&data_mutex);
    msgs_sent++;

    /* Schedule display update (deferred to work queue for thread safety) */
    if (index == selected_keyboard) {
        schedule_display_update();
    }

    return 0;
}

int scanner_msg_send_swipe(int direction) {
    LOG_DBG("Swipe gesture: direction=%d", direction);
    msgs_sent++;
    return 0;
}

int scanner_msg_send_tap(int16_t x, int16_t y) {
    LOG_DBG("Tap: x=%d, y=%d", x, y);
    msgs_sent++;
    return 0;
}

int scanner_msg_send_battery_update(void) {
    /* Skip during Pong Wars to avoid LVGL thread conflicts */
    if (pong_wars_active) {
        return 0;
    }

    /* Read local scanner battery from ZMK battery API */
    int scanner_battery_level = 0;

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    scanner_battery_level = zmk_battery_state_of_charge();
#endif

    /* Only update if we got a valid reading */
    if (scanner_battery_level > 0) {
        display_update_scanner_battery(scanner_battery_level);
    }

    msgs_sent++;
    return 0;
}

int scanner_msg_send_timeout_check(void) {
    /* Check for timed-out keyboards */
    if (!mutex_initialized) return 0;

    uint32_t now = k_uptime_get_32();
    const uint32_t timeout_ms = 10000;  /* 10 seconds */

    if (k_mutex_lock(&data_mutex, K_MSEC(5)) != 0) {
        return -EBUSY;
    }

    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            if ((now - keyboards[i].last_seen) > timeout_ms) {
                LOG_INF("Keyboard in slot %d timed out", i);
                keyboards[i].active = false;
                keyboards[i].name[0] = '\0';
            }
        }
    }

    k_mutex_unlock(&data_mutex);
    msgs_sent++;
    return 0;
}

int scanner_msg_send_display_refresh(void) {
    if (scanner_get_active_keyboard_count() > 0) {
        schedule_display_update();
    }
    msgs_sent++;
    return 0;
}

int scanner_msg_send_timeout_wake(void) {
    msgs_sent++;
    return 0;
}

int scanner_msg_send_brightness_sensor_read(void) {
    msgs_sent++;
    return 0;
}

int scanner_msg_send_brightness_set_target(uint8_t target_brightness) {
    msgs_sent++;
    return 0;
}

int scanner_msg_send_brightness_fade_step(void) {
    msgs_sent++;
    return 0;
}

int scanner_msg_send_brightness_set_auto(bool enabled) {
    msgs_sent++;
    return 0;
}

int scanner_msg_get(void *msg, k_timeout_t timeout) {
    (void)msg;
    (void)timeout;
    return -ENOMSG;
}

void scanner_msg_purge(void) {
    k_msgq_purge(&scanner_msgq);
}

void scanner_msg_get_stats(uint32_t *sent, uint32_t *dropped, uint32_t *processed) {
    if (sent) *sent = msgs_sent;
    if (dropped) *dropped = msgs_dropped;
    if (processed) *processed = msgs_processed;
}

void scanner_msg_increment_processed(void) {
    msgs_processed++;
}

uint32_t scanner_msg_get_queue_count(void) {
    return k_msgq_num_used_get(&scanner_msgq);
}

/* ========== Scanner Start (delayed after boot) ========== */

static void scanner_start_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(scanner_start_work, scanner_start_work_handler);

static void scanner_start_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    LOG_INF("Starting BLE scanner...");
    int ret = zmk_status_scanner_start();
    if (ret == 0) {
        LOG_INF("BLE scanner started successfully");
    } else {
        LOG_ERR("Failed to start BLE scanner: %d", ret);
        /* Retry after 1 second */
        k_work_schedule(&scanner_start_work, K_SECONDS(1));
    }
}

static int scanner_init_start(void) {
    /* Schedule scanner start after 500ms to allow BLE to initialize */
    k_work_schedule(&scanner_start_work, K_MSEC(500));
    return 0;
}

SYS_INIT(scanner_init_start, APPLICATION, 98);
