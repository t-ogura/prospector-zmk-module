/**
 * Scanner Message Handler - Connects BLE scanner to display widgets
 *
 * Receives keyboard advertisement data from status_scanner.c and
 * stores it for the display to render.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zmk/status_advertisement.h>
#include <zmk/status_scanner.h>
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
static uint8_t selected_keyboard_ble_addr[6] = {0};  /* BLE address for reliable matching */
static bool selected_keyboard_addr_valid = false;     /* True when BLE address is set */
static struct k_mutex data_mutex;
static bool mutex_initialized = false;

/* ========== Pending Display Data (thread-safe flag-based update) ========== */
/* Work queue sets data + flag, LVGL timer in main thread processes it */

struct pending_display_data {
    /* Flags - set by work queue, cleared by LVGL timer */
    volatile bool update_pending;
    volatile bool signal_update_pending;  /* Signal widget updates separately (1Hz) */
    volatile bool no_keyboards;           /* True when all keyboards timed out */

    /* Cached data for LVGL update */
    char device_name[MAX_NAME_LEN];
    char layer_name[8];    /* Layer name from Periodic ADV (v2.2.0) */
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
    /* Copy data (atomic enough for our use case) */
    *out = pending_data;
    pending_data.update_pending = false;
    /* Note: signal_update_pending is checked separately via scanner_get_pending_signal */
    return true;
}

/* Global signal data - set by work handler, read DIRECTLY by timer callback */
/* Completely avoiding float function parameters */
volatile int8_t scanner_signal_rssi = -100;
volatile int32_t scanner_signal_rate_x100 = -100;  /* rate * 100, as integer */

/* Called by work handler to set signal data */
static void set_signal_data(int8_t rssi, float rate_hz) {
    scanner_signal_rssi = rssi;
    scanner_signal_rate_x100 = (int32_t)(rate_hz * 100.0f);
}

/* Check if signal update is pending (no data returned - caller reads globals directly) */
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

/* Get keyboard data by BLE address - fixes index mismatch between status_scanner and local array */
bool scanner_get_keyboard_data_by_addr(const uint8_t *ble_addr, struct zmk_status_adv_data *data,
                                        int8_t *rssi, char *name, size_t name_len, int *found_index) {
    if (!mutex_initialized || !ble_addr) {
        return false;
    }

    if (k_mutex_lock(&data_mutex, K_MSEC(10)) != 0) {
        return false;
    }

    bool result = false;
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && memcmp(keyboards[i].ble_addr, ble_addr, 6) == 0) {
            if (data) *data = keyboards[i].data;
            if (rssi) *rssi = keyboards[i].rssi;
            if (name && name_len > 0) {
                strncpy(name, keyboards[i].name, name_len - 1);
                name[name_len - 1] = '\0';
            }
            if (found_index) *found_index = i;
            result = true;
            break;
        }
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

/* Update keyboard name by BLE address - called from status_scanner.c when SCAN_RSP arrives */
void scanner_update_keyboard_name_by_addr(const uint8_t *ble_addr, const char *name) {
    if (!mutex_initialized || !ble_addr || !name) return;

    if (k_mutex_lock(&data_mutex, K_MSEC(5)) != 0) {
        return;
    }

    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && memcmp(keyboards[i].ble_addr, ble_addr, 6) == 0) {
            /* Only update if current name is empty or generic */
            if (keyboards[i].name[0] == '\0' ||
                strncmp(keyboards[i].name, "Keyboard ", 9) == 0 ||
                strcmp(keyboards[i].name, "Unknown") == 0) {
                strncpy(keyboards[i].name, name, MAX_NAME_LEN - 1);
                keyboards[i].name[MAX_NAME_LEN - 1] = '\0';
                LOG_INF("scanner_stub: Updated keyboard name: %s (slot %d)", name, i);
            }
            break;
        }
    }

    k_mutex_unlock(&data_mutex);
}

int scanner_get_selected_keyboard(void) {
    return selected_keyboard;
}

bool scanner_get_selected_keyboard_addr(uint8_t *ble_addr_out) {
    if (!selected_keyboard_addr_valid || !ble_addr_out) {
        return false;
    }
    memcpy(ble_addr_out, selected_keyboard_ble_addr, 6);
    return true;
}

/* Forward declaration */
static void schedule_display_update(void);

void scanner_set_selected_keyboard(int index) {
    if (index >= 0 && index < MAX_KEYBOARDS) {
        selected_keyboard = index;

        /* Get BLE address from status_scanner.c (the authoritative source)
         * This ensures we match by BLE address even if indices differ between arrays */
        struct zmk_keyboard_status *kb = zmk_status_scanner_get_keyboard(index);
        if (kb && kb->active) {
            memcpy(selected_keyboard_ble_addr, kb->ble_addr, 6);
            selected_keyboard_addr_valid = true;
            LOG_INF("Selected keyboard slot %d: %s (BLE=%02X:%02X:%02X:%02X:%02X:%02X, ch=%d, v%s)",
                    index, kb->ble_name,
                    kb->ble_addr[5], kb->ble_addr[4], kb->ble_addr[3],
                    kb->ble_addr[2], kb->ble_addr[1], kb->ble_addr[0],
                    kb->data.channel, kb->has_periodic ? "2" : "1");
        } else {
            /* Fallback: try scanner_stub's local array */
            if (k_mutex_lock(&data_mutex, K_MSEC(5)) == 0) {
                if (keyboards[index].active) {
                    memcpy(selected_keyboard_ble_addr, keyboards[index].ble_addr, 6);
                    selected_keyboard_addr_valid = true;
                    LOG_INF("Selected keyboard slot %d (from local): %s (BLE=%02X:%02X:%02X:%02X:%02X:%02X)",
                            index, keyboards[index].name,
                            keyboards[index].ble_addr[5], keyboards[index].ble_addr[4],
                            keyboards[index].ble_addr[3], keyboards[index].ble_addr[2],
                            keyboards[index].ble_addr[1], keyboards[index].ble_addr[0]);
                } else {
                    LOG_WRN("Selected keyboard slot %d is NOT ACTIVE!", index);
                    selected_keyboard_addr_valid = false;
                }
                k_mutex_unlock(&data_mutex);
            } else {
                LOG_WRN("Selected keyboard slot %d (couldn't verify)", index);
                selected_keyboard_addr_valid = false;
            }
        }

        /* Initiate Periodic Sync if this is a v2 keyboard (v2.2.0) */
        /* This also updates selected_keyboard_index in status_scanner.c */
        int sync_result = zmk_status_scanner_select_keyboard(index);
        if (sync_result == 0) {
            LOG_INF("📡 Periodic sync initiation requested for keyboard %d", index);
        } else if (sync_result == -ENOTSUP) {
            LOG_INF("📡 Keyboard %d is v1 - using Legacy mode", index);
        } else {
            LOG_WRN("📡 Periodic sync failed for keyboard %d: %d", index, sync_result);
        }

        /* Immediately update display with new keyboard data */
        schedule_display_update();
    }
}

/* ========== Display Update Work (runs in system work queue context) ========== */

/* Rate calculation state - using actual advertisement reception count */
static atomic_t adv_receive_count = ATOMIC_INIT(0);  /* Incremented on each adv reception */
static uint32_t rate_last_calc_time = 0;
static int8_t last_rssi = -100;

/* Moving average for smooth rate display */
#define RATE_HISTORY_SIZE 4
static float rate_history[RATE_HISTORY_SIZE] = {0};
static int rate_history_idx = 0;
static bool rate_history_filled = false;  /* True after first full cycle */

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
    int local_index = -1;
    bool keyboard_found = false;

    /* Primary: Look up by BLE address (fixes index mismatch between arrays) */
    if (selected_keyboard_addr_valid) {
        keyboard_found = scanner_get_keyboard_data_by_addr(
            selected_keyboard_ble_addr, &data, &rssi, name, sizeof(name), &local_index);
        if (keyboard_found) {
            /* Update selected_keyboard to local array index for consistency */
            selected_keyboard = local_index;
            LOG_DBG("Keyboard found by BLE addr at local index %d", local_index);
        }
    }

    /* Fallback: Look up by index (for backward compatibility) */
    if (!keyboard_found) {
        keyboard_found = scanner_get_keyboard_data(selected_keyboard, &data, &rssi, name, sizeof(name));
    }

    if (!keyboard_found) {
        /* Check if any keyboard is active */
        int active_count = scanner_get_active_keyboard_count();
        if (active_count == 0) {
            /* All keyboards timed out - trigger "Scanning..." display */
            LOG_INF("No active keyboards - returning to Scanning... state");
            pending_data.no_keyboards = true;
            pending_data.update_pending = true;

            /* Reset signal data */
            set_signal_data(-100, -1.0f);
            pending_data.signal_update_pending = true;

            /* Reset rate calculation state */
            rate_last_calc_time = 0;
            atomic_set(&adv_receive_count, 0);
            rate_history_filled = false;
            rate_history_idx = 0;
        } else {
            /* Try to switch to another active keyboard */
            for (int i = 0; i < MAX_KEYBOARDS; i++) {
                if (i != selected_keyboard) {
                    struct zmk_status_adv_data tmp_data;
                    if (scanner_get_keyboard_data(i, &tmp_data, NULL, NULL, 0)) {
                        selected_keyboard = i;
                        LOG_INF("Switched to keyboard slot %d", i);
                        /* Reschedule to update with new keyboard */
                        k_work_schedule(&display_update_work, K_MSEC(10));
                        return;
                    }
                }
            }
        }
        return;
    }

    /* Keyboard data available - clear no_keyboards flag */
    pending_data.no_keyboards = false;

    /* Only log when data actually changes to reduce spam */
    static uint8_t last_layer = 0xFF;
    static uint8_t last_bat = 0xFF;
    if (data.active_layer != last_layer || data.battery_level != last_bat) {
        LOG_INF("Display update: %s, Layer=%d, Battery=%d%%",
                name, data.active_layer, data.battery_level);
        last_layer = data.active_layer;
        last_bat = data.battery_level;
    }

    /* Store data in pending structure - NO LVGL calls here! */
    strncpy(pending_data.device_name, name, MAX_NAME_LEN - 1);
    pending_data.device_name[MAX_NAME_LEN - 1] = '\0';
    pending_data.layer = data.active_layer;

    /* Layer name: try to get from status_scanner.c (has Periodic ADV layer_names) */
    pending_data.layer_name[0] = '\0';  /* Default: empty */
    if (selected_keyboard_addr_valid) {
        struct zmk_keyboard_status *kb = zmk_status_scanner_get_keyboard_by_addr(selected_keyboard_ble_addr);
        if (kb && kb->layer_count > 0) {
            int layer_idx = data.active_layer;
            if (layer_idx >= 0 && layer_idx < kb->layer_count &&
                kb->layer_names[layer_idx][0] != '\0') {
                strncpy(pending_data.layer_name, kb->layer_names[layer_idx],
                        sizeof(pending_data.layer_name) - 1);
                pending_data.layer_name[sizeof(pending_data.layer_name) - 1] = '\0';
            }
        }
    }
    /* Fallback to legacy layer name if no Periodic ADV name */
    if (pending_data.layer_name[0] == '\0' && data.layer_name[0] != '\0') {
        strncpy(pending_data.layer_name, data.layer_name, sizeof(pending_data.layer_name) - 1);
        pending_data.layer_name[sizeof(pending_data.layer_name) - 1] = '\0';
    }

    pending_data.wpm = data.wpm_value;
    pending_data.usb_ready = (data.status_flags & ZMK_STATUS_FLAG_USB_HID_READY) != 0;
    pending_data.ble_connected = (data.status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED) != 0;
    pending_data.ble_bonded = (data.status_flags & ZMK_STATUS_FLAG_BLE_BONDED) != 0;
    pending_data.profile = data.profile_slot;
    pending_data.modifiers = data.modifier_flags;
    pending_data.bat[0] = data.battery_level;
    pending_data.bat[1] = data.peripheral_battery[0];
    pending_data.bat[2] = data.peripheral_battery[1];
    pending_data.bat[3] = data.peripheral_battery[2];

    /* Calculate reception rate from actual advertisement count (1Hz update with moving average) */
    last_rssi = rssi;

    /* Initialize rate calculation on first call */
    if (rate_last_calc_time == 0) {
        rate_last_calc_time = now;
        /* Don't reset counter - let advertisements that arrived before init be counted */
    }

    /* Check if 1 second has passed since last calculation */
    uint32_t elapsed_since_calc = now - rate_last_calc_time;

    if (elapsed_since_calc >= 1000) {
        /* 1 second elapsed - calculate rate from actual advertisement receptions */
        uint32_t elapsed = now - rate_last_calc_time;
        int count = atomic_get(&adv_receive_count);
        atomic_set(&adv_receive_count, 0);  /* Reset for next interval */

        /* Calculate instantaneous rate */
        float instant_rate = (float)count * 1000.0f / (float)elapsed;

        /* Add to moving average history */
        rate_history[rate_history_idx] = instant_rate;
        rate_history_idx = (rate_history_idx + 1) % RATE_HISTORY_SIZE;
        if (rate_history_idx == 0) {
            rate_history_filled = true;
        }

        /* Calculate moving average */
        float avg_rate = 0.0f;
        int samples = rate_history_filled ? RATE_HISTORY_SIZE : rate_history_idx;
        if (samples == 0) samples = 1;  /* Prevent division by zero */
        for (int i = 0; i < samples; i++) {
            avg_rate += rate_history[i];
        }
        avg_rate /= (float)samples;

        /* Set signal data via global variables (avoids float pointer issues) */
        set_signal_data(rssi, avg_rate);
        pending_data.signal_update_pending = true;  /* Signal widget updates at 1Hz */
        rate_last_calc_time = now;
    }

    /* Set flag - LVGL timer in main thread will pick this up */
    pending_data.update_pending = true;
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

/**
 * @brief High-priority display update for Periodic Advertising data
 *
 * Called from status_scanner.c when Periodic ADV data arrives.
 * Reads directly from status_scanner.c's keyboard array (authoritative source)
 * and populates pending_data for immediate display refresh.
 *
 * Unlike Legacy ADV which stores data locally, Periodic data is only in
 * status_scanner.c's keyboards[] array, so we read from there.
 */
void scanner_trigger_high_priority_update(void) {
    /* Debug: track call frequency */
    static uint32_t call_count = 0;
    call_count++;

    /* Skip when Pong Wars is active */
    if (pong_wars_active) {
        if (call_count <= 3) LOG_DBG("HIGH_PRIO: skip pong_wars");
        return;
    }

    /* Skip if screen transition in progress */
    if (transition_in_progress) {
        if (call_count <= 3) LOG_DBG("HIGH_PRIO: skip transition");
        return;
    }

    /* Get selected keyboard index from status_scanner.c */
    int idx = zmk_status_scanner_get_selected_keyboard();
    if (idx < 0) {
        if (call_count <= 3) LOG_WRN("HIGH_PRIO: idx=%d (no keyboard selected)", idx);
        return;
    }

    /* Get keyboard data from status_scanner.c (authoritative source for Periodic data) */
    struct zmk_keyboard_status *kb = zmk_status_scanner_get_keyboard(idx);
    if (!kb || !kb->active) {
        if (call_count <= 3) LOG_WRN("HIGH_PRIO: kb=%p, active=%d", kb, kb ? kb->active : -1);
        return;
    }

    /* Debug: Log first few successful calls */
    if (call_count <= 3) {
        LOG_INF("HIGH_PRIO[%u]: idx=%d, layer=%d ✓", call_count, idx, kb->data.active_layer);
    }

    /* Populate pending_data directly - NO LVGL calls! */
    strncpy(pending_data.device_name, kb->ble_name, MAX_NAME_LEN - 1);
    pending_data.device_name[MAX_NAME_LEN - 1] = '\0';
    pending_data.layer = kb->data.active_layer;

    /* Layer name: prefer Periodic ADV layer_names, fallback to legacy layer_name */
    int layer_idx = kb->data.active_layer;
    if (kb->layer_count > 0 && layer_idx >= 0 && layer_idx < kb->layer_count &&
        kb->layer_names[layer_idx][0] != '\0') {
        /* Use layer name from Periodic ADV static packet */
        strncpy(pending_data.layer_name, kb->layer_names[layer_idx], sizeof(pending_data.layer_name) - 1);
    } else if (kb->data.layer_name[0] != '\0') {
        /* Fallback to legacy layer name (4 chars max) */
        strncpy(pending_data.layer_name, kb->data.layer_name, sizeof(pending_data.layer_name) - 1);
    } else {
        /* No layer name available - will use default in display */
        pending_data.layer_name[0] = '\0';
    }
    pending_data.layer_name[sizeof(pending_data.layer_name) - 1] = '\0';

    pending_data.wpm = kb->data.wpm_value;
    pending_data.usb_ready = (kb->data.status_flags & ZMK_STATUS_FLAG_USB_HID_READY) != 0;
    pending_data.ble_connected = (kb->data.status_flags & ZMK_STATUS_FLAG_BLE_CONNECTED) != 0;
    pending_data.ble_bonded = (kb->data.status_flags & ZMK_STATUS_FLAG_BLE_BONDED) != 0;
    pending_data.profile = kb->data.profile_slot;
    pending_data.modifiers = kb->data.modifier_flags;
    pending_data.bat[0] = kb->data.battery_level;
    pending_data.bat[1] = kb->data.peripheral_battery[0];
    pending_data.bat[2] = kb->data.peripheral_battery[1];
    pending_data.bat[3] = kb->data.peripheral_battery[2];
    pending_data.rssi = kb->rssi;

    /* Update signal data with new RSSI (rate calculation remains separate) */
    last_rssi = kb->rssi;

    /* Set flags for immediate update */
    pending_data.no_keyboards = false;
    pending_data.update_pending = true;

    /* Debug: Log when Periodic triggers high-priority update (only on layer change) */
    static uint8_t last_periodic_layer = 0xFF;
    if (kb->data.active_layer != last_periodic_layer) {
        LOG_INF("⚡ PERIODIC UPDATE: Layer=%d (was %d)",
                kb->data.active_layer, last_periodic_layer);
        last_periodic_layer = kb->data.active_layer;
    }

    /* Also sync to local keyboards array so scanner_get_keyboard_data* functions work */
    if (k_mutex_lock(&data_mutex, K_MSEC(2)) == 0) {
        /* Find or allocate slot in local array */
        int local_idx = -1;
        for (int i = 0; i < MAX_KEYBOARDS; i++) {
            if (keyboards[i].active &&
                memcmp(keyboards[i].ble_addr, kb->ble_addr, 6) == 0) {
                local_idx = i;
                break;
            }
        }
        if (local_idx >= 0) {
            memcpy(&keyboards[local_idx].data, &kb->data, sizeof(struct zmk_status_adv_data));
            keyboards[local_idx].rssi = kb->rssi;
            keyboards[local_idx].last_seen = kb->last_seen;
        }
        k_mutex_unlock(&data_mutex);
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

    /* Update name: Only overwrite if we have a real name (not "Unknown")
     * This prevents flickering when ADV packet arrives before SCAN_RSP */
    if (device_name && device_name[0] != '\0') {
        if (keyboards[index].name[0] == '\0') {
            /* First time - set whatever we have */
            strncpy(keyboards[index].name, device_name, MAX_NAME_LEN - 1);
            keyboards[index].name[MAX_NAME_LEN - 1] = '\0';
        } else if (strcmp(device_name, "Unknown") != 0 &&
                   strcmp(keyboards[index].name, device_name) != 0) {
            /* Real name received - update from Unknown/generic to actual name */
            strncpy(keyboards[index].name, device_name, MAX_NAME_LEN - 1);
            keyboards[index].name[MAX_NAME_LEN - 1] = '\0';
            LOG_INF("Updated keyboard name: %s (slot %d)", device_name, index);
        }
        /* If device_name is "Unknown" but we already have a real name, don't overwrite */
    } else if (keyboards[index].name[0] == '\0') {
        snprintf(keyboards[index].name, MAX_NAME_LEN, "Keyboard %d", index);
    }

    k_mutex_unlock(&data_mutex);
    msgs_sent++;

    /* Check if this advertisement is from the selected keyboard
     * Use BLE address matching for reliability (indices may differ between arrays) */
    bool is_selected = false;
    if (selected_keyboard_addr_valid && ble_addr != NULL) {
        /* Primary: Match by BLE address */
        is_selected = (memcmp(ble_addr, selected_keyboard_ble_addr, 6) == 0);
    } else {
        /* Fallback: Match by index (may be unreliable if arrays are out of sync) */
        is_selected = (index == selected_keyboard);
    }

    LOG_DBG("ADV: idx=%d, sel=%d, BLE match=%s, ch=%d",
            index, selected_keyboard,
            is_selected ? "YES" : "NO",
            adv_data->channel);

    /* Count advertisement reception for rate calculation */
    if (is_selected) {
        /* Update selected_keyboard index to match local array for display_update_work_handler */
        selected_keyboard = index;
        atomic_inc(&adv_receive_count);
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

    /* Use CONFIG value for timeout (synced with status_scanner.c) */
#ifdef CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS
    const uint32_t timeout_ms = CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS;
#else
    const uint32_t timeout_ms = 480000;  /* 8 minutes default */
#endif

    /* Skip if timeout is disabled */
    if (timeout_ms == 0) {
        return 0;
    }

    if (k_mutex_lock(&data_mutex, K_MSEC(5)) != 0) {
        return -EBUSY;
    }

    bool any_timed_out = false;
    for (int i = 0; i < MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            if ((now - keyboards[i].last_seen) > timeout_ms) {
                LOG_INF("Keyboard in slot %d timed out", i);
                keyboards[i].active = false;
                keyboards[i].name[0] = '\0';
                any_timed_out = true;
            }
        }
    }

    k_mutex_unlock(&data_mutex);

    /* Trigger display update if any keyboard timed out */
    if (any_timed_out) {
        schedule_display_update();
    }

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
