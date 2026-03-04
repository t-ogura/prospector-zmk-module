/**
 * Scanner Message Handler - Connects BLE scanner to display widgets
 *
 * Architecture (lock-free):
 *   BT RX thread → ring buffer push (non-blocking, no mutex)
 *   LVGL timer (100ms) → scanner_process_incoming() → drain ring buffer
 *                       → manage keyboards[] → set pending_data → widget update
 *
 * keyboards[] is accessed ONLY from LVGL timer context (single-threaded).
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zmk/status_scanner.h>
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

/* ========== Keyboard Data Storage ========== */
/* Accessed ONLY from LVGL timer context (main thread) */
/* Uses struct zmk_keyboard_status from zmk/status_scanner.h as single source of truth */

#define MAX_KEYBOARDS ZMK_STATUS_SCANNER_MAX_KEYBOARDS
#define MAX_NAME_LEN 32

static struct zmk_keyboard_status keyboards[MAX_KEYBOARDS];
static int selected_keyboard = 0;

/* ========== SPSC Ring Buffer (BT RX → LVGL timer) ========== */

#define INCOMING_BUF_SIZE 16  /* Must be power of 2 */

struct incoming_adv {
    struct zmk_status_adv_data data;
    int8_t rssi;
    char name[MAX_NAME_LEN];
    uint8_t ble_addr[6];
    uint8_t ble_addr_type;
};

static struct incoming_adv incoming_buf[INCOMING_BUF_SIZE];
static volatile uint8_t incoming_write_idx;  /* BT RX only writes */
static volatile uint8_t incoming_read_idx;   /* LVGL timer only writes */

/* Push one entry from BT RX thread (producer) */
static int incoming_push(const struct incoming_adv *entry) {
    uint8_t next = (incoming_write_idx + 1) & (INCOMING_BUF_SIZE - 1);
    if (next == incoming_read_idx) {
        return -ENOMEM;  /* Buffer full, drop */
    }
    incoming_buf[incoming_write_idx] = *entry;
    __DMB();  /* ARM memory barrier: ensure data written before index update */
    incoming_write_idx = next;
    return 0;
}

/* Pop one entry from LVGL timer context (consumer) */
static bool incoming_pop(struct incoming_adv *out) {
    if (incoming_read_idx == incoming_write_idx) {
        return false;  /* Empty */
    }
    *out = incoming_buf[incoming_read_idx];
    __DMB();  /* ARM memory barrier: ensure data read before index update */
    incoming_read_idx = (incoming_read_idx + 1) & (INCOMING_BUF_SIZE - 1);
    return true;
}

/* ========== Pending Display Data (set by LVGL timer, read by LVGL timer) ========== */

struct pending_display_data {
    volatile bool update_pending;
    volatile bool signal_update_pending;  /* Signal widget updates separately (1Hz) */
    volatile bool no_keyboards;           /* True when all keyboards timed out */

    char device_name[MAX_NAME_LEN];
    char layer_name[4];
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

static struct pending_display_data pending_data = {0};

/* Getter for pending data - called from LVGL timer in main thread */
bool scanner_get_pending_update(struct pending_display_data *out) {
    if (!pending_data.update_pending) {
        return false;
    }
    *out = pending_data;
    pending_data.update_pending = false;
    return true;
}

/* Global signal data - set by process_incoming, read by timer callback */
volatile int8_t scanner_signal_rssi = -100;
volatile int32_t scanner_signal_rate_x100 = -100;  /* rate * 100, as integer */

static void set_signal_data(int8_t rssi, float rate_hz) {
    scanner_signal_rssi = rssi;
    scanner_signal_rate_x100 = (int32_t)(rate_hz * 100.0f);
}

/* Check if signal update is pending */
bool scanner_is_signal_pending(void) {
    if (!pending_data.signal_update_pending) {
        return false;
    }
    pending_data.signal_update_pending = false;
    return true;
}

/* Check if scanner battery update is pending */
bool scanner_get_pending_battery(int *level) {
    if (!pending_data.scanner_battery_pending) {
        return false;
    }
    *level = pending_data.scanner_battery;
    pending_data.scanner_battery_pending = false;
    return true;
}

/* ========== Public API for Display (LVGL timer context only) ========== */

bool scanner_get_keyboard_data(int index, struct zmk_status_adv_data *data,
                               int8_t *rssi, char *name, size_t name_len) {
    if (index < 0 || index >= MAX_KEYBOARDS) {
        return false;
    }

    if (!keyboards[index].active) {
        return false;
    }

    if (data) *data = keyboards[index].data;
    if (rssi) *rssi = keyboards[index].rssi;
    if (name && name_len > 0) {
        strncpy(name, keyboards[index].ble_name, name_len - 1);
        name[name_len - 1] = '\0';
    }
    return true;
}

int scanner_get_active_keyboard_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) count++;
    }
    return count;
}

int scanner_get_selected_keyboard(void) {
    return selected_keyboard;
}

struct zmk_keyboard_status *scanner_get_keyboard_status(int index) {
    if (index < 0 || index >= MAX_KEYBOARDS) {
        return NULL;
    }
    return keyboards[index].active ? &keyboards[index] : NULL;
}

/* Helper: populate pending_data from keyboards[selected_keyboard] */
static void fill_pending_from_selected(void) {
    struct zmk_status_adv_data *d;
    if (selected_keyboard < 0 || selected_keyboard >= MAX_KEYBOARDS ||
        !keyboards[selected_keyboard].active) {
        return;
    }

    d = &keyboards[selected_keyboard].data;
    strncpy(pending_data.device_name, keyboards[selected_keyboard].ble_name, MAX_NAME_LEN - 1);
    pending_data.device_name[MAX_NAME_LEN - 1] = '\0';
    memcpy(pending_data.layer_name, d->layer_name, sizeof(pending_data.layer_name));
    pending_data.layer = d->active_layer;
    pending_data.wpm = d->wpm_value;
    pending_data.usb_ready = (d->status_flags & ZMK_STATUS_FLAG_USB_HID_READY) != 0;
    pending_data.ble_connected = (d->status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED) != 0;
    pending_data.ble_bonded = (d->status_flags & ZMK_STATUS_FLAG_BLE_BONDED) != 0;
    pending_data.profile = d->profile_slot;
    pending_data.modifiers = d->modifier_flags;
    pending_data.bat[0] = d->battery_level;
    pending_data.bat[1] = d->peripheral_battery[0];
    pending_data.bat[2] = d->peripheral_battery[1];
    pending_data.bat[3] = d->peripheral_battery[2];
    pending_data.no_keyboards = false;
    pending_data.update_pending = true;
}

void scanner_set_selected_keyboard(int index) {
    if (index >= 0 && index < MAX_KEYBOARDS) {
        selected_keyboard = index;
        LOG_INF("Selected keyboard changed to slot %d", index);
        fill_pending_from_selected();
    }
}

/* ========== Rate Calculation State ========== */

static atomic_t adv_receive_count = ATOMIC_INIT(0);  /* Incremented by BT RX (atomic) */
static uint32_t rate_last_calc_time = 0;

#define RATE_HISTORY_SIZE 4
static float rate_history[RATE_HISTORY_SIZE] = {0};
static int rate_history_idx = 0;
static bool rate_history_filled = false;

/* Scanner battery update interval */
static uint32_t scanner_battery_last_update = 0;
#define SCANNER_BATTERY_UPDATE_INTERVAL_MS 5000

/* Timeout check interval counter */
static uint32_t process_call_count = 0;

/* ========== scanner_process_incoming() - Called from LVGL timer ========== */

void scanner_process_incoming(void) {
    struct incoming_adv entry;
    bool selected_updated = false;

    /* 1. Drain ring buffer: process all pending advertisements */
    while (incoming_pop(&entry)) {
        /* Find existing keyboard or empty slot */
        int index = -1;
        uint32_t keyboard_id = (entry.data.keyboard_id[0] << 24) |
                               (entry.data.keyboard_id[1] << 16) |
                               (entry.data.keyboard_id[2] << 8) |
                               entry.data.keyboard_id[3];

        /* PRIORITY 1: BLE address match */
        for (int i = 0; i < MAX_KEYBOARDS; i++) {
            if (keyboards[i].active &&
                memcmp(keyboards[i].ble_addr, entry.ble_addr, 6) == 0) {
                index = i;
                break;
            }
        }

        /* PRIORITY 2: keyboard_id + device_role match */
        if (index < 0) {
            uint8_t incoming_role = entry.data.device_role;
            for (int i = 0; i < MAX_KEYBOARDS; i++) {
                if (keyboards[i].active) {
                    uint32_t stored_id = (keyboards[i].data.keyboard_id[0] << 24) |
                                         (keyboards[i].data.keyboard_id[1] << 16) |
                                         (keyboards[i].data.keyboard_id[2] << 8) |
                                         keyboards[i].data.keyboard_id[3];
                    if (stored_id == keyboard_id &&
                        keyboards[i].data.device_role == incoming_role) {
                        index = i;
                        if (memcmp(keyboards[i].ble_addr, entry.ble_addr, 6) != 0) {
                            LOG_INF("stub: slot %d BLE addr updated (ID=%08X)", i, keyboard_id);
                            memcpy(keyboards[i].ble_addr, entry.ble_addr, 6);
                        }
                        break;
                    }
                }
            }
        }

        /* PRIORITY 3: Empty slot */
        if (index < 0) {
            for (int i = 0; i < MAX_KEYBOARDS; i++) {
                if (!keyboards[i].active) {
                    index = i;
                    LOG_INF("stub: new slot %d: %s (ID=%08X)",
                           index, entry.name[0] ? entry.name : "(null)", keyboard_id);
                    break;
                }
            }
        }

        if (index < 0) {
            LOG_WRN("No slot for keyboard ID=%08X", keyboard_id);
            continue;
        }

        /* Store the data */
        keyboards[index].active = true;
        memcpy(&keyboards[index].data, &entry.data, sizeof(struct zmk_status_adv_data));
        keyboards[index].rssi = entry.rssi;
        keyboards[index].last_seen = k_uptime_get_32();
        memcpy(keyboards[index].ble_addr, entry.ble_addr, 6);
        keyboards[index].ble_addr_type = entry.ble_addr_type;

        /* Update name: preserve real name, don't overwrite with "Unknown" */
        if (entry.name[0] != '\0') {
            if (keyboards[index].ble_name[0] == '\0') {
                strncpy(keyboards[index].ble_name, entry.name, MAX_NAME_LEN - 1);
                keyboards[index].ble_name[MAX_NAME_LEN - 1] = '\0';
            } else if (strcmp(entry.name, "Unknown") != 0 &&
                       strcmp(keyboards[index].ble_name, entry.name) != 0) {
                strncpy(keyboards[index].ble_name, entry.name, MAX_NAME_LEN - 1);
                keyboards[index].ble_name[MAX_NAME_LEN - 1] = '\0';
                LOG_INF("Updated keyboard name: %s (slot %d)", entry.name, index);
            }
        } else if (keyboards[index].ble_name[0] == '\0') {
            snprintf(keyboards[index].ble_name, MAX_NAME_LEN, "Keyboard %d", index);
        }

        if (index == selected_keyboard) {
            selected_updated = true;
        }
    }

    /* 2. Set pending_data from selected keyboard */
    if (selected_updated) {
        fill_pending_from_selected();
    }

    /* 3. Rate calculation (1Hz) */
    uint32_t now = k_uptime_get_32();

    if (rate_last_calc_time == 0) {
        rate_last_calc_time = now;
    }

    uint32_t elapsed_since_calc = now - rate_last_calc_time;
    if (elapsed_since_calc >= 1000) {
        int count = atomic_get(&adv_receive_count);
        atomic_set(&adv_receive_count, 0);

        float instant_rate = (float)count * 1000.0f / (float)elapsed_since_calc;

        rate_history[rate_history_idx] = instant_rate;
        rate_history_idx = (rate_history_idx + 1) % RATE_HISTORY_SIZE;
        if (rate_history_idx == 0) {
            rate_history_filled = true;
        }

        float avg_rate = 0.0f;
        int samples = rate_history_filled ? RATE_HISTORY_SIZE : rate_history_idx;
        if (samples == 0) samples = 1;
        for (int i = 0; i < samples; i++) {
            avg_rate += rate_history[i];
        }
        avg_rate /= (float)samples;

        int8_t rssi = (selected_keyboard >= 0 && selected_keyboard < MAX_KEYBOARDS &&
                       keyboards[selected_keyboard].active)
                      ? keyboards[selected_keyboard].rssi : -100;

        set_signal_data(rssi, avg_rate);
        pending_data.signal_update_pending = true;
        rate_last_calc_time = now;
    }

    /* 4. Timeout check (every ~10 calls = ~1 second at 100ms timer) */
    process_call_count++;
    if ((process_call_count % 10) == 0) {
#ifdef CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS
        const uint32_t timeout_ms = CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS;
#else
        const uint32_t timeout_ms = 480000;
#endif
        if (timeout_ms > 0) {
            bool any_timed_out = false;
            for (int i = 0; i < MAX_KEYBOARDS; i++) {
                if (keyboards[i].active &&
                    (now - keyboards[i].last_seen) > timeout_ms) {
                    LOG_INF("Keyboard in slot %d timed out", i);
                    keyboards[i].active = false;
                    keyboards[i].ble_name[0] = '\0';
                    any_timed_out = true;
                }
            }

            if (any_timed_out) {
                int active_count = scanner_get_active_keyboard_count();
                if (active_count == 0) {
                    LOG_INF("No active keyboards - returning to Scanning... state");
                    pending_data.no_keyboards = true;
                    pending_data.update_pending = true;
                    set_signal_data(-100, -1.0f);
                    pending_data.signal_update_pending = true;
                    rate_last_calc_time = 0;
                    atomic_set(&adv_receive_count, 0);
                    rate_history_filled = false;
                    rate_history_idx = 0;
                } else {
                    /* Selected keyboard timed out - switch to another */
                    if (!keyboards[selected_keyboard].active) {
                        for (int i = 0; i < MAX_KEYBOARDS; i++) {
                            if (keyboards[i].active) {
                                selected_keyboard = i;
                                LOG_INF("Switched to keyboard slot %d", i);
                                fill_pending_from_selected();
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    /* 5. Scanner battery (every ~5 seconds) */
    if (scanner_battery_last_update == 0 ||
        (now - scanner_battery_last_update) >= SCANNER_BATTERY_UPDATE_INTERVAL_MS) {
        int scanner_battery_level = 0;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
        scanner_battery_level = zmk_battery_state_of_charge();
#endif
        if (scanner_battery_level > 0) {
            pending_data.scanner_battery = scanner_battery_level;
            pending_data.scanner_battery_pending = true;
        }
        scanner_battery_last_update = now;
    }
}

/* ========== Scanner Message Functions ========== */

int scanner_msg_send_keyboard_data(const struct zmk_status_adv_data *adv_data,
                                   int8_t rssi, const char *device_name,
                                   const uint8_t *ble_addr, uint8_t ble_addr_type) {
    struct incoming_adv entry;
    memcpy(&entry.data, adv_data, sizeof(struct zmk_status_adv_data));
    entry.rssi = rssi;

    if (device_name && device_name[0] != '\0') {
        strncpy(entry.name, device_name, MAX_NAME_LEN - 1);
        entry.name[MAX_NAME_LEN - 1] = '\0';
    } else {
        entry.name[0] = '\0';
    }

    if (ble_addr) {
        memcpy(entry.ble_addr, ble_addr, 6);
        entry.ble_addr_type = ble_addr_type;
    } else {
        memset(entry.ble_addr, 0, 6);
        entry.ble_addr_type = 0;
    }

    int ret = incoming_push(&entry);
    if (ret != 0) {
        LOG_WRN("Ring buffer full, advertisement dropped");
        return ret;
    }

    /* Count for rate calculation (atomic, safe from any thread) */
    atomic_inc(&adv_receive_count);

    return 0;
}

int scanner_msg_send_swipe(int direction) {
    LOG_DBG("Swipe gesture: direction=%d", direction);
    return 0;
}

int scanner_msg_send_tap(int16_t x, int16_t y) {
    LOG_DBG("Tap: x=%d, y=%d", x, y);
    return 0;
}

int scanner_msg_send_battery_update(void) {
    /* Scanner battery is now handled by scanner_process_incoming() */
    return 0;
}

int scanner_msg_send_timeout_check(void) {
    /* Timeouts are now handled by scanner_process_incoming() */
    return 0;
}

int scanner_msg_send_display_refresh(void) {
    /* Called from LVGL timer context (swipe handler) when returning to MAIN screen.
     * Fill pending_data directly from current keyboard state. */
    if (scanner_get_active_keyboard_count() > 0) {
        fill_pending_from_selected();
    }
    return 0;
}

int scanner_msg_send_timeout_wake(void) {
    return 0;
}

int scanner_msg_send_brightness_sensor_read(void) {
    return 0;
}

int scanner_msg_send_brightness_set_target(uint8_t target_brightness) {
    return 0;
}

int scanner_msg_send_brightness_fade_step(void) {
    return 0;
}

int scanner_msg_send_brightness_set_auto(bool enabled) {
    return 0;
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
        k_work_schedule(&scanner_start_work, K_SECONDS(1));
    }
}

static int scanner_init_start(void) {
    k_work_schedule(&scanner_start_work, K_MSEC(500));
    return 0;
}

SYS_INIT(scanner_init_start, APPLICATION, 98);
