/**
 * Scanner Message Handler - Connects BLE scanner to display widgets
 *
 * Architecture (ring buffer + work handler, matching v2.1.0 timing):
 *   BT RX thread → ring buffer push (non-blocking, no mutex)
 *   Work handler (100ms) → scanner_process_incoming() → drain ring buffer
 *                         → manage keyboards[] → set pending_data
 *   LVGL timer (100ms)   → read pending_data → widget update (display only)
 *
 * Key design: Data processing (work handler) is separated from display
 * rendering (LVGL timer), matching v2.1.0's proven architecture.
 * The ring buffer eliminates mutex on the BT RX hot path.
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
/* Protected by data_mutex: work handler writes, LVGL timer reads */
/* Uses struct zmk_keyboard_status from zmk/status_scanner.h as single source of truth */

#define MAX_KEYBOARDS ZMK_STATUS_SCANNER_MAX_KEYBOARDS
#define MAX_NAME_LEN 32

static struct zmk_keyboard_status keyboards[MAX_KEYBOARDS];
static int selected_keyboard = 0;
static struct k_mutex data_mutex;
static bool mutex_initialized = false;

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
static volatile uint8_t incoming_read_idx;   /* Work handler only writes */

/* Push one entry from BT RX thread (producer) */
static int incoming_push(const struct incoming_adv *entry) {
    uint8_t wi = incoming_write_idx & (INCOMING_BUF_SIZE - 1);
    uint8_t next = (wi + 1) & (INCOMING_BUF_SIZE - 1);
    uint8_t ri = incoming_read_idx & (INCOMING_BUF_SIZE - 1);
    if (next == ri) {
        return -ENOMEM;  /* Buffer full, drop */
    }
    incoming_buf[wi] = *entry;
    __DMB();  /* ARM memory barrier: ensure data written before index update */
    incoming_write_idx = next;
    return 0;
}

/* Pop one entry from LVGL timer context (consumer) */
static bool incoming_pop(struct incoming_adv *out) {
    uint8_t ri = incoming_read_idx & (INCOMING_BUF_SIZE - 1);
    uint8_t wi = incoming_write_idx & (INCOMING_BUF_SIZE - 1);
    if (ri == wi) {
        return false;  /* Empty */
    }
    *out = incoming_buf[ri];
    __DMB();  /* ARM memory barrier: ensure data read before index update */
    incoming_read_idx = (ri + 1) & (INCOMING_BUF_SIZE - 1);
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

    /* Keyboard firmware version (decoded from version + profile_slot fields) */
    uint8_t kb_version_major;
    uint8_t kb_version_minor;
    uint8_t kb_version_patch;
    bool kb_version_dev;
    bool kb_version_valid;       /* True after first keyboard data received */
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

static void set_signal_data(int8_t rssi, int32_t rate_x100) {
    scanner_signal_rssi = rssi;
    scanner_signal_rate_x100 = rate_x100;
}

/* Check if signal update is pending */
bool scanner_is_signal_pending(void) {
    if (!pending_data.signal_update_pending) {
        return false;
    }
    pending_data.signal_update_pending = false;
    return true;
}

/* Get keyboard firmware version (from last received advertisement) */
bool scanner_get_kb_version(uint8_t *major, uint8_t *minor, uint8_t *patch,
                            bool *is_dev, char *name, size_t name_len) {
    if (!pending_data.kb_version_valid) {
        return false;
    }
    if (major) *major = pending_data.kb_version_major;
    if (minor) *minor = pending_data.kb_version_minor;
    if (patch) *patch = pending_data.kb_version_patch;
    if (is_dev) *is_dev = pending_data.kb_version_dev;
    if (name && name_len > 0) {
        strncpy(name, pending_data.device_name, name_len - 1);
        name[name_len - 1] = '\0';
    }
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

/* ========== Public API for Display ========== */
/* All functions accessing keyboards[] are mutex-protected (matching v2.2a) */

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
            strncpy(name, keyboards[index].ble_name, name_len - 1);
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

struct zmk_keyboard_status *scanner_get_keyboard_status(int index) {
    /* Note: Returns pointer under mutex protection. Caller must not hold
     * the pointer across long operations. For keyboard selection screen,
     * copy the data immediately. */
    if (!mutex_initialized || index < 0 || index >= MAX_KEYBOARDS) {
        return NULL;
    }

    if (k_mutex_lock(&data_mutex, K_MSEC(10)) != 0) {
        return NULL;
    }

    struct zmk_keyboard_status *result = keyboards[index].active ? &keyboards[index] : NULL;

    k_mutex_unlock(&data_mutex);
    return result;
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
    pending_data.profile = PROSPECTOR_DECODE_PROFILE(d->profile_slot);
    pending_data.modifiers = d->modifier_flags;
    pending_data.bat[0] = d->battery_level;
    pending_data.bat[1] = d->peripheral_battery[0];
    pending_data.bat[2] = d->peripheral_battery[1];
    pending_data.bat[3] = d->peripheral_battery[2];

    /* Decode keyboard firmware version */
    pending_data.kb_version_major = PROSPECTOR_DECODE_VERSION_MAJOR(d->version);
    pending_data.kb_version_minor = PROSPECTOR_DECODE_VERSION_MINOR(d->version);
    pending_data.kb_version_patch = PROSPECTOR_DECODE_PATCH(d->profile_slot);
    pending_data.kb_version_dev = PROSPECTOR_DECODE_DEV(d->profile_slot);
    pending_data.kb_version_valid = true;

    pending_data.no_keyboards = false;
    pending_data.update_pending = true;
}

void scanner_set_selected_keyboard(int index) {
    if (index >= 0 && index < MAX_KEYBOARDS) {
        if (mutex_initialized && k_mutex_lock(&data_mutex, K_MSEC(10)) == 0) {
            selected_keyboard = index;
            LOG_INF("Selected keyboard changed to slot %d", index);
            fill_pending_from_selected();
            k_mutex_unlock(&data_mutex);
        }
    }
}

/* ========== Periodic Process Work (drains ring buffer, updates keyboards[]) ========== */
/* Runs in system work queue context, separated from LVGL rendering thread */

static void process_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(process_work, process_work_handler);
static volatile bool process_pending = false;

static void schedule_process(void) {
    if (!process_pending) {
        process_pending = true;
        /* Batch rapid advertisements with 50ms delay (same as v2.1.0) */
        k_work_schedule(&process_work, K_MSEC(50));
    }
}

/* ========== Rate Calculation State ========== */

static atomic_t adv_receive_count = ATOMIC_INIT(0);  /* Incremented by BT RX (atomic) */
static uint32_t rate_last_calc_time = 0;

#define RATE_HISTORY_SIZE 4
static int32_t rate_history_x100[RATE_HISTORY_SIZE] = {0};  /* rate × 100, integer */
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

    /* 1. Drain ring buffer: process all pending advertisements
     * Bounded to INCOMING_BUF_SIZE to prevent infinite loop if indices are corrupted */
    int drain_limit = INCOMING_BUF_SIZE;
    while (drain_limit-- > 0 && incoming_pop(&entry)) {
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

        /* Integer-only rate calculation: rate_x100 = count * 100000 / elapsed_ms */
        int32_t instant_rate_x100 = (elapsed_since_calc > 0)
            ? (int32_t)((uint32_t)count * 100000U / elapsed_since_calc)
            : 0;

        rate_history_x100[rate_history_idx] = instant_rate_x100;
        rate_history_idx = (rate_history_idx + 1) % RATE_HISTORY_SIZE;
        if (rate_history_idx == 0) {
            rate_history_filled = true;
        }

        int32_t avg_rate_x100 = 0;
        int samples = rate_history_filled ? RATE_HISTORY_SIZE : rate_history_idx;
        if (samples == 0) samples = 1;
        for (int i = 0; i < samples; i++) {
            avg_rate_x100 += rate_history_x100[i];
        }
        avg_rate_x100 /= samples;

        int8_t rssi = (selected_keyboard >= 0 && selected_keyboard < MAX_KEYBOARDS &&
                       keyboards[selected_keyboard].active)
                      ? keyboards[selected_keyboard].rssi : -100;

        set_signal_data(rssi, avg_rate_x100);
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
                    set_signal_data(-100, -100);
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

/* ========== Process Work Handler ========== */

static void process_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    process_pending = false;

    if (mutex_initialized && k_mutex_lock(&data_mutex, K_MSEC(50)) == 0) {
        scanner_process_incoming();
        k_mutex_unlock(&data_mutex);
    }

    /* Reschedule periodically (rate calc, timeout checks, battery) */
    k_work_schedule(&process_work, K_MSEC(100));
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

    /* Trigger work handler to process (batched with 50ms delay like v2.1.0) */
    schedule_process();

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
    if (!mutex_initialized) return 0;
    if (k_mutex_lock(&data_mutex, K_MSEC(10)) != 0) return 0;

    int active = 0;
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) active++;
    }
    if (active > 0) {
        fill_pending_from_selected();
    }

    k_mutex_unlock(&data_mutex);
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
    k_mutex_init(&data_mutex);
    mutex_initialized = true;
    k_work_schedule(&scanner_start_work, K_MSEC(500));
    /* Start periodic processing after scanner starts */
    k_work_schedule(&process_work, K_MSEC(600));
    return 0;
}

SYS_INIT(scanner_init_start, APPLICATION, 98);
