/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/hwinfo.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/usb.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/status_advertisement.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>

// keymap API is available on Central or Standalone (non-Split) builds only
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/keymap.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
#include <zmk/behavior.h>
#endif

// Custom WPM implementation to avoid ZMK WPM compatibility issues
static uint32_t key_press_count = 0;
static uint32_t wpm_start_time = 0;
static uint8_t current_wpm = 0;
// Circular buffer for rolling window implementation
#define WPM_HISTORY_SIZE 60  // 60 seconds of history at 1-second resolution
static uint8_t wpm_key_history[WPM_HISTORY_SIZE] = {0};  // Keys per second
static uint32_t wpm_history_index = 0;  // Current position in circular buffer
static uint32_t wpm_last_second = 0;    // Last second we recorded
static uint8_t wpm_current_second_keys = 0;  // Keys in current second

// WPM calculation configuration - using Kconfig settings
// Backward compatibility: provide defaults if Kconfig values not defined
#ifndef CONFIG_ZMK_STATUS_ADV_WPM_WINDOW_SECONDS
#define CONFIG_ZMK_STATUS_ADV_WPM_WINDOW_SECONDS 30  // 30 seconds default
#endif

#ifndef CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS
#define CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS 0  // Auto-calculate default
#endif

// Handle common typo: WMP instead of WPM
// Priority: Use WPM if set, otherwise fallback to WMP if set, otherwise default
#ifdef CONFIG_ZMK_STATUS_ADV_WMP_DECAY_TIMEOUT_SECONDS
#if CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS == 0 && CONFIG_ZMK_STATUS_ADV_WMP_DECAY_TIMEOUT_SECONDS != 0
// If WPM is default (0) but WMP has a value, use WMP value
#undef CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS
#define CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS CONFIG_ZMK_STATUS_ADV_WMP_DECAY_TIMEOUT_SECONDS
#endif
#endif

// Calculate window parameters from Kconfig
#define WPM_WINDOW_MS (CONFIG_ZMK_STATUS_ADV_WPM_WINDOW_SECONDS * 1000)
#define WPM_WINDOW_MULTIPLIER ((CONFIG_ZMK_STATUS_ADV_WPM_WINDOW_SECONDS > 0) ? \
                              (60 / CONFIG_ZMK_STATUS_ADV_WPM_WINDOW_SECONDS) : 2)  // Auto-calculate multiplier, fallback to 2
// Calculate decay timeout: auto (2x window) or manual
#define WPM_DECAY_TIMEOUT_MS ((CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS == 0) ? \
                              (WPM_WINDOW_MS * 2) : \
                              ((CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS >= 10) ? \
                               (CONFIG_ZMK_STATUS_ADV_WPM_DECAY_TIMEOUT_SECONDS * 1000) : \
                               (WPM_WINDOW_MS * 2)))

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_STATUS_ADVERTISEMENT)

// HYBRID APPROACH: Piggyback when disconnected, own ADV when connected
// - Disconnected: bt_le_adv_update_data() injects into ZMK's SCAN_RSP
// - Connected: bt_le_adv_start() with non-connectable ADV (no NRPA)
// - Disconnect callback stops own ADV before ZMK restarts
#pragma message "*** PROSPECTOR HYBRID ADVERTISING ***"

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// No additional includes needed - zmk_peripheral_battery_state_changed is in battery_state_changed.h
#endif

static struct k_work_delayable adv_work;
static bool adv_started = false;
static enum zmk_activity_state last_activity_state = ZMK_ACTIVITY_ACTIVE;

// Hybrid state tracking
static bool zmk_adv_was_active = false;        // Piggyback mode: ZMK is advertising
static bool prospector_adv_active = false;     // Our own ADV is running (MODE 2 or proxy)
static bool prospector_adv_connectable = false; // true = connectable proxy, false = non-connectable MODE 2

// Adaptive update intervals based on activity - using Kconfig values for flexibility
#if IS_ENABLED(CONFIG_ZMK_STATUS_ADV_ACTIVITY_BASED)
#define ACTIVE_UPDATE_INTERVAL_MS   CONFIG_ZMK_STATUS_ADV_ACTIVE_INTERVAL_MS    // Configurable active interval
#define IDLE_UPDATE_INTERVAL_MS     CONFIG_ZMK_STATUS_ADV_IDLE_INTERVAL_MS      // Configurable idle interval
#define ACTIVITY_TIMEOUT_MS         CONFIG_ZMK_STATUS_ADV_ACTIVITY_TIMEOUT_MS   // Configurable timeout
#else
// Fallback to legacy fixed interval if activity-based is disabled
#define ACTIVE_UPDATE_INTERVAL_MS   CONFIG_ZMK_STATUS_ADV_INTERVAL_MS
#define IDLE_UPDATE_INTERVAL_MS     CONFIG_ZMK_STATUS_ADV_INTERVAL_MS
#define ACTIVITY_TIMEOUT_MS         10000   // 10 seconds default
#endif

static uint32_t last_activity_time = 0;
static bool is_active = false;

// Burst advertisement for high-priority events (layer/modifier changes)
// Sends multiple advertisements in quick succession to ensure scanner receives at least one
#define BURST_COUNT 5           // Number of advertisements per burst
#define BURST_INTERVAL_MS 15    // Interval between burst advertisements
static atomic_t burst_remaining = ATOMIC_INIT(0);

// Latest layer state for accurate tracking (unused currently)
// static uint8_t latest_layer = 0;

// Peripheral battery tracking for split keyboards
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static uint8_t peripheral_batteries[3] = {0, 0, 0}; // Up to 3 peripheral devices
static int peripheral_battery_listener(const zmk_event_t *eh);

ZMK_LISTENER(prospector_peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(prospector_peripheral_battery, zmk_peripheral_battery_state_changed);
#endif

// Activity-based update system: key presses trigger high-frequency updates
static int position_state_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev && ev->state) { // Only on key press (not release)
        uint32_t now = k_uptime_get_32();
        last_activity_time = now;

        bool was_active = is_active;
        is_active = true;

        // Debug activity state transitions
        if (!was_active) {
            LOG_INF("⚡ ACTIVITY: Switched to ACTIVE mode - now using %dms intervals (10Hz)", ACTIVE_UPDATE_INTERVAL_MS);
        }

        // Track keys per second for rolling window
        key_press_count++;

        // Update circular buffer when second changes
        uint32_t current_second = now / 1000;
        if (current_second != wpm_last_second) {
            // Move to next second in circular buffer
            uint32_t seconds_elapsed = current_second - wpm_last_second;

            // Fill in any missed seconds with 0
            for (uint32_t i = 0; i < seconds_elapsed && i < WPM_HISTORY_SIZE; i++) {
                wpm_history_index = (wpm_history_index + 1) % WPM_HISTORY_SIZE;
                wpm_key_history[wpm_history_index] = (i == 0) ? wpm_current_second_keys : 0;
            }

            wpm_last_second = current_second;
            wpm_current_second_keys = 0;
        }

        wpm_current_second_keys++;

        LOG_DBG("🔥 Key activity detected - switching to high frequency updates");

        // Immediately trigger update if switching from idle to active
        if (!was_active && adv_started) {
            k_work_cancel_delayable(&adv_work);
            k_work_schedule(&adv_work, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prospector_position_listener, position_state_listener);
ZMK_SUBSCRIPTION(prospector_position_listener, zmk_position_state_changed);

// Profile change listener for immediate advertisement updates
static int profile_changed_listener(const zmk_event_t *eh) {
    LOG_DBG("📡 BLE profile changed - updating advertisement");
    if (adv_started) {
        k_work_cancel_delayable(&adv_work);
        k_work_schedule(&adv_work, K_NO_WAIT);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
ZMK_LISTENER(prospector_profile_listener, profile_changed_listener);
ZMK_SUBSCRIPTION(prospector_profile_listener, zmk_ble_active_profile_changed);
#endif

// Layer change listener for immediate advertisement updates
// Triggers on both activation AND deactivation for responsive display updates
// Uses burst mode to send multiple advertisements for reliability
static int layer_changed_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev) {
        // Trigger on both press (state=true) and release (state=false)
        LOG_DBG("🔄 Layer %d %s - triggering burst advertisement (%d×%dms)",
                ev->layer, ev->state ? "activated" : "deactivated",
                BURST_COUNT, BURST_INTERVAL_MS);
        if (adv_started) {
            atomic_set(&burst_remaining, BURST_COUNT);
            k_work_cancel_delayable(&adv_work);
            k_work_schedule(&adv_work, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
// Layer events only available on Central or non-Split devices
ZMK_LISTENER(prospector_layer_listener, layer_changed_listener);
ZMK_SUBSCRIPTION(prospector_layer_listener, zmk_layer_state_changed);
#endif

// Modifier change listener for immediate advertisement updates
// Triggers on both press AND release for responsive display updates
// NOTE: No burst mode for modifiers - they change too frequently and burst
// would block scan response (device name) from being sent
static int modifiers_changed_listener(const zmk_event_t *eh) {
    const struct zmk_modifiers_state_changed *ev = as_zmk_modifiers_state_changed(eh);
    if (ev) {
        // Trigger on both press (state=true) and release (state=false)
        // Single immediate update only (no burst) to allow scan responses
        LOG_DBG("🎹 Modifiers %s (0x%02x) - triggering immediate advertisement",
                ev->state ? "pressed" : "released", ev->modifiers);
        if (adv_started) {
            // Don't use burst for modifiers - they're too frequent
            k_work_cancel_delayable(&adv_work);
            k_work_schedule(&adv_work, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prospector_modifiers_listener, modifiers_changed_listener);
ZMK_SUBSCRIPTION(prospector_modifiers_listener, zmk_modifiers_state_changed);

// Activity state listener for sleep/wake handling
// This ensures proper advertising restart after system sleep
static int activity_state_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    enum zmk_activity_state new_state = ev->state;

    LOG_INF("🔄 Activity state changed: %s -> %s",
            last_activity_state == ZMK_ACTIVITY_ACTIVE ? "ACTIVE" :
            last_activity_state == ZMK_ACTIVITY_IDLE ? "IDLE" : "SLEEP",
            new_state == ZMK_ACTIVITY_ACTIVE ? "ACTIVE" :
            new_state == ZMK_ACTIVITY_IDLE ? "IDLE" : "SLEEP");

    // Handle sleep entry - stop work handler and own ADV if running
    if (new_state == ZMK_ACTIVITY_SLEEP) {
        LOG_INF("💤 Entering sleep - stopping Prospector updates");
        k_work_cancel_delayable(&adv_work);
        if (prospector_adv_active) {
            bt_le_adv_stop();
            prospector_adv_active = false;
        }
        zmk_adv_was_active = false;
    }
    // Handle wake from sleep - restart work handler
    else if (new_state == ZMK_ACTIVITY_ACTIVE &&
             last_activity_state == ZMK_ACTIVITY_SLEEP) {
        LOG_INF("⚡ Waking from sleep - restarting Prospector updates");
        if (adv_started) {
            k_work_cancel_delayable(&adv_work);
            k_work_schedule(&adv_work, K_MSEC(500));  // Wait for BLE to stabilize
            LOG_INF("⚡ Prospector update scheduled (500ms delay)");
        }
    }

    last_activity_state = new_state;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(prospector_activity_listener, activity_state_listener);
ZMK_SUBSCRIPTION(prospector_activity_listener, zmk_activity_state_changed);

// WPM is now handled by custom implementation in position_state_listener
// No separate WPM event listener needed - integrated into position_state_listener

// Use ZMK's correct API for profile detection
static uint8_t get_active_profile_slot(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    return zmk_ble_active_profile_index();
#else
    return 0;  // Peripheral or non-BLE device
#endif
}

// Helper function to determine current update interval
static uint32_t get_current_update_interval(void) {
    uint32_t now = k_uptime_get_32();

    // Check if we should transition from active to idle
    if (is_active && (now - last_activity_time) > ACTIVITY_TIMEOUT_MS) {
        is_active = false;
        uint32_t idle_duration = now - last_activity_time;
        LOG_INF("💤 ACTIVITY: Switched to IDLE mode after %dms - now using %dms intervals (1Hz)",
                idle_duration, IDLE_UPDATE_INTERVAL_MS);
    }

    // Check connection states using reliable APIs
    bool ble_connected = false;
    bool usb_connected = false;

#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    // Use ZMK BLE API to check connection status
    ble_connected = zmk_ble_active_profile_is_connected();
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
    // Check if USB is connected and HID ready (reliable API)
    usb_connected = zmk_usb_is_hid_ready();
#endif

    // Determine interval based on connection state and activity
    uint32_t interval;
    if (!ble_connected && !usb_connected) {
        // Not connected at all - use idle rate regardless of activity (save power)
        interval = IDLE_UPDATE_INTERVAL_MS;
        LOG_DBG("Not connected - using idle interval: %dms", interval);
    } else {
        // Connected (BLE or USB) - use activity-based intervals
        interval = is_active ? ACTIVE_UPDATE_INTERVAL_MS : IDLE_UPDATE_INTERVAL_MS;
        LOG_DBG("Connected (%s) - update interval: %dms (%s mode)",
                ble_connected ? "BLE" : "USB", interval, is_active ? "ACTIVE" : "IDLE");
    }

    return interval;
}


// BLE Legacy Advertising 31-byte limit: Flags(3) + Manufacturer(2+N) <= 31
// Therefore: max manufacturer payload = 31 - 3 - 2 = 26 bytes
#define MAX_ADV_DATA_LEN     31
#define FLAGS_LEN           3   // 1 length + 1 type + 1 data
#define MANUF_OVERHEAD      2   // 1 length + 1 type
#define MAX_MANUF_PAYLOAD   (MAX_ADV_DATA_LEN - FLAGS_LEN - MANUF_OVERHEAD) // = 26

// Ensure our structure matches the expected 26-byte payload
_Static_assert(sizeof(struct zmk_status_adv_data) == MAX_MANUF_PAYLOAD,
               "zmk_status_adv_data must be exactly 26 bytes");

static struct zmk_status_adv_data manufacturer_data; // Use structured data directly

// =====================================================================
// HYBRID ADVERTISING
// =====================================================================
// MODE 1 (disconnected): Piggyback on ZMK's advertising via bt_le_adv_update_data()
//   - Injects manufacturer data into ZMK's SCAN_RSP
//   - Zero interference with ZMK's BLE stack
// MODE 2 (connected): Own non-connectable ADV via bt_le_adv_start()
//   - ZMK stops advertising when connected, so we use the advertising set
//   - Non-connectable, no NRPA (preserves BLE address state)
//   - Stopped immediately on disconnect (bt_conn_cb) before ZMK restarts
// =====================================================================

// --- MODE 1: Piggyback data ---
// Name placement depends on Zephyr version:
//   FORCE_NAME_IN_AD available (cormoran fork): name in AD → SD free for manufacturer
//   FORCE_NAME_IN_AD absent (upstream Zephyr): name in SD → manufacturer must go in AD
// Without this separation, 28-byte manufacturer + name in SD exceeds 31-byte limit → name truncated
#if defined(BT_LE_ADV_OPT_FORCE_NAME_IN_AD)
// Newer Zephyr: ZMK uses FORCE_NAME_IN_AD → name in AD, SD free for manufacturer data
static struct bt_data zmk_ad_restore[] = {
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, BT_BYTES_LIST_LE16(CONFIG_BT_DEVICE_APPEARANCE)),
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME,
                  BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL),
                  BT_UUID_16_ENCODE(BT_UUID_BAS_VAL)),
};

static struct bt_data piggyback_sd[] = {
    BT_DATA(BT_DATA_MANUFACTURER_DATA, (uint8_t *)&manufacturer_data, sizeof(manufacturer_data)),
};
#endif
// Older Zephyr (no FORCE_NAME_IN_AD): piggyback uses prospector_ad for AD, NULL for SD.
// Zephyr auto-appends device name to SD. Scanner gets manufacturer from AD, name from SD.

// --- MODE 2: Own non-connectable ADV data ---
static struct bt_data prospector_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, (uint8_t *)&manufacturer_data, sizeof(manufacturer_data)),
};

// --- Name-in-AD: Periodic name broadcast ---
// MODE 2 SCAN_RSP doesn't reach scanner (radio busy with BLE connection).
// Periodically swap AD data to send device name directly in ADV packet.
#define NAME_ADV_INTERVAL_ACTIVE  25  // Every 25th cycle (~5s at 200ms)
#define NAME_ADV_INTERVAL_IDLE     1  // Every cycle when idle (30s interval)
static uint32_t adv_cycle_counter = 0;
static char name_adv_buffer[29]; // Max name in 31-byte AD: 31 - 2(flags) = 29
static struct bt_data name_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, name_adv_buffer, 0), // Length set dynamically
};

// Non-connectable ADV params (MODE 2: when active profile IS connected)
// SCANNABLE + USE_NAME: scanner can get device name via SCAN_RSP
// Without SCANNABLE, ADV_NONCONN_IND has no SCAN_RSP → name never reaches scanner
static const struct bt_le_adv_param prospector_adv_params = {
#if defined(BT_LE_ADV_OPT_SCANNABLE)
    .options = BT_LE_ADV_OPT_SCANNABLE | BT_LE_ADV_OPT_USE_NAME,
#else
    .options = 0,  // Fallback for older Zephyr without SCANNABLE
#endif
    .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,  // 100ms
    .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,  // 150ms
};

// Connectable proxy ADV params (when active profile is NOT connected)
// Must match ZMK's ZMK_ADV_CONN_NAME so bonded PCs can reconnect.
// ZMK's update_advertising() may have failed with -EALREADY because our
// MODE 2 was occupying the advertising set during profile switch.
// Uses BT_LE_ADV_OPT_CONNECTABLE (not BT_LE_ADV_OPT_CONN) for Zephyr compatibility.
static const struct bt_le_adv_param proxy_connectable_params = {
    .options = BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_USE_NAME,
    .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,  // 100ms
    .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,  // 150ms
};

// --- Disconnect callback: stop own ADV before ZMK restarts ---
static void prospector_ble_disconnected(struct bt_conn *conn, uint8_t reason) {
    if (prospector_adv_active) {
        bt_le_adv_stop();
        prospector_adv_active = false;
        LOG_INF("📡 Disconnect detected - stopped own ADV for ZMK handoff");
    }
}

static struct bt_conn_cb prospector_conn_callbacks = {
    .disconnected = prospector_ble_disconnected,
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// Peripheral battery event listener for split keyboards
static int peripheral_battery_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    if (ev) {
        LOG_DBG("Peripheral %d battery: %d%%", ev->source, ev->state_of_charge);
        if (ev->source < 3) {
            peripheral_batteries[ev->source] = ev->state_of_charge;
        }
        // Trigger immediate status update when peripheral battery changes
        if (adv_started) {
            k_work_cancel_delayable(&adv_work);
            k_work_schedule(&adv_work, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
#endif

static void build_manufacturer_payload(void) {
    // Build 26-byte structured manufacturer data
    memset(&manufacturer_data, 0, sizeof(manufacturer_data));

    // Apply WPM decay based on inactivity
    uint32_t now = k_uptime_get_32();
    uint32_t time_since_activity = now - last_activity_time;

    // Calculate WPM from circular buffer (rolling window)
    uint32_t current_second = now / 1000;

    // Update circular buffer when second changes (even without key presses)
    if (current_second != wpm_last_second && wpm_last_second > 0) {
        uint32_t seconds_elapsed = current_second - wpm_last_second;

        // Fill in any missed seconds with 0
        for (uint32_t i = 0; i < seconds_elapsed && i < WPM_HISTORY_SIZE; i++) {
            wpm_history_index = (wpm_history_index + 1) % WPM_HISTORY_SIZE;
            wpm_key_history[wpm_history_index] = (i == 0) ? wpm_current_second_keys : 0;
        }

        wpm_last_second = current_second;
        wpm_current_second_keys = 0;
    }

    // Initialize last_second if first time
    if (wpm_last_second == 0) {
        wpm_last_second = current_second;
    }

    // Calculate WPM from last N seconds (configurable window)
    uint32_t window_keys = 0;
    uint32_t window_seconds = CONFIG_ZMK_STATUS_ADV_WPM_WINDOW_SECONDS;

    // Sum keys from the last window_seconds
    for (uint32_t i = 0; i < window_seconds && i < WPM_HISTORY_SIZE; i++) {
        uint32_t idx = (wpm_history_index + WPM_HISTORY_SIZE - i) % WPM_HISTORY_SIZE;
        window_keys += wpm_key_history[idx];
    }

    // Add current second's keys (not yet in buffer)
    window_keys += wpm_current_second_keys;

    // Calculate WPM: (keys * 60) / (window_seconds * 5)
    // Simplified: (keys * 12) / window_seconds
    // But we use the multiplier for correct scaling
    if (window_seconds > 0 && window_keys > 0) {
        uint32_t new_wpm = (window_keys * 12 * WPM_WINDOW_MULTIPLIER) / window_seconds;

        // Smooth transition (avoid jumps)
        if (current_wpm == 0 || abs((int)new_wpm - (int)current_wpm) > 50) {
            current_wpm = new_wpm;  // Big change or first value - use directly
        } else {
            // Small change - apply smoothing
            current_wpm = (new_wpm * 7 + current_wpm * 3) / 10;
        }

        if (current_wpm > 255) current_wpm = 255;

        LOG_DBG("📊 WPM calculated: %d (keys: %d, window: %ds, mult: %dx)",
                current_wpm, window_keys, window_seconds, WPM_WINDOW_MULTIPLIER);
    }

    if (time_since_activity > WPM_DECAY_TIMEOUT_MS) {
        // Reset WPM after timeout
        current_wpm = 0;
        memset(wpm_key_history, 0, sizeof(wpm_key_history));
        wpm_history_index = 0;
        wpm_current_second_keys = 0;
        wpm_last_second = 0;
        LOG_DBG("📊 WPM reset due to %dms inactivity", WPM_DECAY_TIMEOUT_MS);
    } else if (time_since_activity > 5000 && current_wpm > 0) {
        // Apply smooth decay after 5 seconds of inactivity
        float idle_seconds = (time_since_activity - 5000) / 1000.0f;
        // Adjust decay rate for configurable window (faster decay for shorter window)
        float decay_factor = 1.0f - (idle_seconds / (WPM_WINDOW_MS / 1000.0f));
        if (decay_factor < 0.0f) decay_factor = 0.0f;

        // Apply decay to current WPM
        uint8_t decayed_wpm = (uint8_t)(current_wpm * decay_factor);
        if (decayed_wpm != current_wpm) {
            LOG_DBG("📊 WPM decay: %d -> %d (idle: %.1fs)",
                    current_wpm, decayed_wpm, idle_seconds + 5.0f);
            current_wpm = decayed_wpm;
        }
    }

    // Fixed header fields
    manufacturer_data.manufacturer_id[0] = 0xFF;
    manufacturer_data.manufacturer_id[1] = 0xFF;
    manufacturer_data.service_uuid[0] = 0xAB;
    manufacturer_data.service_uuid[1] = 0xCD;
    manufacturer_data.version = ZMK_STATUS_ADV_VERSION;

    // Central/Standalone battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    manufacturer_data.battery_level = battery_level;

    // Layer information - proper ZMK split-aware approach
    uint8_t layer = 0;

    /*
     * Central or Standalone (Split disabled): keymap API available
     * Peripheral (Split enabled but not Central): no keymap, layer = 0
     */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    // Central/Standalone: Use keymap API for layer detection
    // active_layer is uint8_t (0-255), no artificial limit needed
    layer = zmk_keymap_highest_layer_active();
#else
    // Peripheral: No keymap available, always layer 0
    // Layer information comes from Central side via split communication
    layer = 0;
#endif

    manufacturer_data.active_layer = layer;

    // Profile slot (0-4) as selected in ZMK's settings
    manufacturer_data.profile_slot = get_active_profile_slot();
    LOG_DBG("📡 Active profile slot: %d", manufacturer_data.profile_slot);

    // Connection count approximation - count active BLE connections + USB
    uint8_t connection_count = 1; // Assume at least one connection (BLE advertising implies connection capability)
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_hid_ready()) {
        connection_count++;
    }
#endif
    manufacturer_data.connection_count = connection_count;

    // Status flags - YADS compatible connection status
    uint8_t flags = 0;

    // USB status flags
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        flags |= ZMK_STATUS_FLAG_USB_CONNECTED;
    }
    if (zmk_usb_is_hid_ready()) {
        flags |= ZMK_STATUS_FLAG_USB_HID_READY;
    }
#endif

    // BLE status flags - use ZMK BLE APIs (only on central or non-split)
#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    if (zmk_ble_active_profile_is_connected()) {
        flags |= ZMK_STATUS_FLAG_BLE_CONNECTED;
    }
    if (!zmk_ble_active_profile_is_open()) {
        flags |= ZMK_STATUS_FLAG_BLE_BONDED;
    }
#endif

    manufacturer_data.status_flags = flags;

    // Device role and peripheral batteries
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    manufacturer_data.device_role = ZMK_DEVICE_ROLE_CENTRAL;
    manufacturer_data.device_index = 0; // Central is always index 0

    // Handle battery placement based on central side configuration
    // Default to "RIGHT" if not configured (backward compatibility)
    const char *central_side = "RIGHT";
#ifdef CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE
    central_side = CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE;
#endif

    // Peripheral index mapping (backward compatible defaults)
#ifndef CONFIG_ZMK_STATUS_ADV_HALF_PERIPHERAL
#define CONFIG_ZMK_STATUS_ADV_HALF_PERIPHERAL 0
#endif
#ifndef CONFIG_ZMK_STATUS_ADV_AUX1_PERIPHERAL
#define CONFIG_ZMK_STATUS_ADV_AUX1_PERIPHERAL 1
#endif
#ifndef CONFIG_ZMK_STATUS_ADV_AUX2_PERIPHERAL
#define CONFIG_ZMK_STATUS_ADV_AUX2_PERIPHERAL 2
#endif

    // Get peripheral batteries using configurable indices
    uint8_t half_battery = peripheral_batteries[CONFIG_ZMK_STATUS_ADV_HALF_PERIPHERAL];
    uint8_t aux1_battery = peripheral_batteries[CONFIG_ZMK_STATUS_ADV_AUX1_PERIPHERAL];
    uint8_t aux2_battery = peripheral_batteries[CONFIG_ZMK_STATUS_ADV_AUX2_PERIPHERAL];

    /*
     * Scanner display mapping:
     *   battery_level         -> LEFT arc on screen
     *   peripheral_battery[0] -> RIGHT arc on screen
     *
     * So we map:
     *   LEFT physical side battery  -> battery_level
     *   RIGHT physical side battery -> peripheral_battery[0]
     */
    if (strcmp(central_side, "LEFT") == 0) {
        // Central is on LEFT physical side
        // battery_level (LEFT arc) = central battery (already in battery_level)
        // peripheral_battery[0] (RIGHT arc) = peripheral half
        manufacturer_data.peripheral_battery[0] = half_battery;    // Right physical -> Right arc
        manufacturer_data.peripheral_battery[1] = aux1_battery;    // Aux1 (e.g., trackball)
        manufacturer_data.peripheral_battery[2] = aux2_battery;    // Aux2
        // battery_level already has central battery, no change needed
    } else {
        // Central is on RIGHT physical side (default)
        // battery_level (LEFT arc) = peripheral half (swap needed)
        // peripheral_battery[0] (RIGHT arc) = central (swap needed)
        uint8_t central_battery = battery_level;
        manufacturer_data.battery_level = half_battery;            // Left physical (peripheral) -> Left arc
        manufacturer_data.peripheral_battery[0] = central_battery; // Right physical (central) -> Right arc
        manufacturer_data.peripheral_battery[1] = aux1_battery;    // Aux1 (e.g., trackball)
        manufacturer_data.peripheral_battery[2] = aux2_battery;    // Aux2
    }

#elif IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    // Peripheral: Skip advertising to preserve split communication
    return;
#else
    manufacturer_data.device_role = ZMK_DEVICE_ROLE_STANDALONE;
    manufacturer_data.device_index = 0;
    memset(manufacturer_data.peripheral_battery, 0, 3);
#endif

    // Compact layer name (4 bytes, NOT null-terminated, full 4 chars usable)
    // Receiver must use %.4s or memcpy+null, never raw %s
    memset(manufacturer_data.layer_name, 0, sizeof(manufacturer_data.layer_name));
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    const char *layer_name = zmk_keymap_layer_name(layer);
    if (layer_name && layer_name[0] != '\0') {
        size_t len = strlen(layer_name);
        if (len > sizeof(manufacturer_data.layer_name)) {
            len = sizeof(manufacturer_data.layer_name);
        }
        memcpy(manufacturer_data.layer_name, layer_name, len);
    } else {
        snprintf(manufacturer_data.layer_name, sizeof(manufacturer_data.layer_name),
                 "L%d", layer % 10);
    }
#else
    // Peripheral: no layer name available
    snprintf(manufacturer_data.layer_name, sizeof(manufacturer_data.layer_name),
             "L%d", layer % 10);
#endif

    // Keyboard ID (4 bytes) - hardware-unique ID from HWINFO (FICR on nRF52840)
    // This ensures the same physical device always has the same ID,
    // even when BLE MAC address changes across profile switches.
    {
        uint8_t hwid[16];
        ssize_t hwid_len = hwinfo_get_device_id(hwid, sizeof(hwid));
        uint32_t id_hash = 0;

        if (hwid_len > 0) {
            // Hash hardware device ID to 4 bytes
            for (ssize_t i = 0; i < hwid_len; i++) {
                id_hash = id_hash * 31 + hwid[i];
            }
            LOG_DBG("keyboard_id from HWINFO (%d bytes): %08X", (int)hwid_len, id_hash);
        } else {
            // Fallback: hash keyboard name (for boards without HWINFO)
            const char *keyboard_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
            if (strlen(keyboard_name) == 0) {
                keyboard_name = CONFIG_BT_DEVICE_NAME;
            }
            for (int i = 0; keyboard_name[i]; i++) {
                id_hash = id_hash * 31 + keyboard_name[i];
            }
            LOG_WRN("HWINFO unavailable, using name-hash for keyboard_id: %08X", id_hash);
        }
        memcpy(manufacturer_data.keyboard_id, &id_hash, 4);
    }

    // Modifier keys status - using exact YADS approach
    uint8_t modifier_flags = 0;

    // Get current keyboard report with null check and memory validation
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    if (report && report != NULL) {
        uint8_t mods = report->body.modifiers;

        // SAFETY: Validate modifier data is reasonable (not corrupted memory)
        if (mods <= 0xFF) {  // Valid HID modifier range
            // Map HID modifiers using YADS constants approach
            // Note: MOD_LCTL=0x01, MOD_RCTL=0x10, etc. (standard HID modifier bits)
            if (mods & (0x01 | 0x10)) modifier_flags |= ZMK_MOD_FLAG_LCTL | ZMK_MOD_FLAG_RCTL;  // MOD_LCTL | MOD_RCTL
            if (mods & (0x02 | 0x20)) modifier_flags |= ZMK_MOD_FLAG_LSFT | ZMK_MOD_FLAG_RSFT;  // MOD_LSFT | MOD_RSFT
            if (mods & (0x04 | 0x40)) modifier_flags |= ZMK_MOD_FLAG_LALT | ZMK_MOD_FLAG_RALT;  // MOD_LALT | MOD_RALT
            if (mods & (0x08 | 0x80)) modifier_flags |= ZMK_MOD_FLAG_LGUI | ZMK_MOD_FLAG_RGUI;  // MOD_LGUI | MOD_RGUI
        }
        // If mods is corrupted, modifier_flags stays 0 (safe default)

        // CRITICAL: Ensure no test code remains that forces modifier flags
        // NO TEST CODE SHOULD BE HERE - removed any periodic flag setting

    }

    manufacturer_data.modifier_flags = modifier_flags;

    // WPM (Words Per Minute) data collection - custom implementation
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    // WPM only available on Central or non-Split devices
    manufacturer_data.wpm_value = current_wpm;
    LOG_DBG("⚡ Custom WPM: %d (key presses: %d)", current_wpm, key_press_count);
#else
    // Peripheral: no WPM data
    manufacturer_data.wpm_value = 0;
#endif

    // Channel number (0 = broadcast to all scanners)
#ifdef CONFIG_PROSPECTOR_CHANNEL
    manufacturer_data.channel = CONFIG_PROSPECTOR_CHANNEL;
#else
    manufacturer_data.channel = 0;  // Default: broadcast to all
#endif

    const char *role_str =
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        "CENTRAL";
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
        "PERIPHERAL";
#else
        "STANDALONE";
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_DBG("Prospector %s: Central %d%%, Peripheral [%d,%d,%d], Layer %d",
            role_str, battery_level,
            peripheral_batteries[0], peripheral_batteries[1], peripheral_batteries[2],
            layer);
#else
    LOG_DBG("Prospector %s: Battery %d%%, Layer %d",
            role_str, battery_level, layer);
#endif
}


// =====================================================================
// Work handler - hybrid: piggyback / own ADV (profile-aware)
// =====================================================================
// Three modes based on state:
//   1. Piggyback: ZMK advertising → inject data into SCAN_RSP
//   2. Non-connectable ADV: Active profile connected → status display only
//   3. Connectable proxy ADV: Active profile NOT connected → let PCs reconnect
//
// Mode 3 fixes the critical profile-switch bug: ZMK's update_advertising()
// is called BEFORE raise_profile_changed_event(), so when MODE 2 is blocking
// the ADV set, ZMK gets -EALREADY and has NO retry mechanism.
// =====================================================================

static void adv_work_handler(struct k_work *work) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    // Peripheral: don't interfere with split communication
    return;
#endif

    build_manufacturer_payload();

    // Determine if active profile needs connectable advertising
    bool active_connected = false;
#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    active_connected = zmk_ble_active_profile_is_connected();
#endif

    if (prospector_adv_active) {
        // Our ADV is running - check if type needs to change
        bool need_connectable = !active_connected;

        if (need_connectable != prospector_adv_connectable) {
            // Profile state changed - switch ADV type
            bt_le_adv_stop();
            prospector_adv_active = false;
            LOG_INF("📡 Profile state changed (%s→%s) - restarting ADV",
                    prospector_adv_connectable ? "connectable" : "non-conn",
                    need_connectable ? "connectable" : "non-conn");
            // Fall through to start new ADV below
        } else {
            // Same type - update AD data.
            // Periodically send device name instead of manufacturer data so scanner
            // can learn the name even when SCAN_RSP is blocked by radio congestion.
            adv_cycle_counter++;
            uint32_t name_interval = is_active ? NAME_ADV_INTERVAL_ACTIVE : NAME_ADV_INTERVAL_IDLE;
            bool in_burst = (atomic_get(&burst_remaining) > 0);
            bool send_name = !in_burst &&
                             (adv_cycle_counter % name_interval == 0) &&
                             name_ad[1].data_len > 0;

            int err;
            if (send_name) {
                err = bt_le_adv_update_data(name_ad, ARRAY_SIZE(name_ad), NULL, 0);
                if (err == 0) {
                    LOG_DBG("📡 Name-in-AD sent: \"%s\"", name_adv_buffer);
                }
            } else {
                err = bt_le_adv_update_data(prospector_ad, ARRAY_SIZE(prospector_ad),
                                            NULL, 0);
            }
            if (err == -EAGAIN) {
                // ADV stopped externally (PC connected through proxy, or ZMK took over)
                prospector_adv_active = false;
                LOG_INF("📡 Own ADV stopped externally (connection established?)");
            } else if (err) {
                LOG_DBG("Own ADV update error: %d", err);
            }
        }
    }

    if (!prospector_adv_active) {
        // Try piggyback on ZMK's advertising
#if defined(BT_LE_ADV_OPT_FORCE_NAME_IN_AD)
        // Newer Zephyr: ZMK puts name in AD → SD is free for manufacturer data
        int err = bt_le_adv_update_data(zmk_ad_restore, ARRAY_SIZE(zmk_ad_restore),
                                        piggyback_sd, ARRAY_SIZE(piggyback_sd));
#else
        // Older Zephyr: name goes in SD → put manufacturer in AD to avoid truncation
        // (31-byte SD can't hold both 28-byte manufacturer data AND device name)
        int err = bt_le_adv_update_data(prospector_ad, ARRAY_SIZE(prospector_ad),
                                        NULL, 0);
#endif

        if (err == 0) {
            if (!zmk_adv_was_active) {
                LOG_INF("📡 Piggyback active (ZMK advertising)");
                zmk_adv_was_active = true;
            }
        } else if (err == -EAGAIN) {
            // ZMK not advertising → start our own ADV
            zmk_adv_was_active = false;

            if (active_connected) {
                // Active profile connected → non-connectable (status display only)
                err = bt_le_adv_start(&prospector_adv_params,
                                      prospector_ad, ARRAY_SIZE(prospector_ad),
                                      NULL, 0);
                if (err == 0) {
                    prospector_adv_active = true;
                    prospector_adv_connectable = false;
                    LOG_INF("📡 MODE 2: Non-connectable ADV (profile connected)");
                } else if (err != -EALREADY) {
                    LOG_WRN("Failed to start non-connectable ADV: %d", err);
                }
            } else {
                // Active profile NOT connected → connectable proxy
                // (ZMK's update_advertising() likely failed with -EALREADY)
                // Manufacturer data in AD, Zephyr auto-appends name to SD
                err = bt_le_adv_start(&proxy_connectable_params,
                                      prospector_ad, ARRAY_SIZE(prospector_ad),
                                      NULL, 0);
                if (err == 0) {
                    prospector_adv_active = true;
                    prospector_adv_connectable = true;
                    LOG_INF("📡 Connectable proxy ADV (profile not connected)");
                } else if (err != -EALREADY) {
                    LOG_WRN("Failed to start connectable proxy ADV: %d", err);
                }
            }
        } else {
            LOG_WRN("📡 Piggyback update error: %d", err);
        }
    }

    // Check if we're in burst mode (high-priority event)
    int remaining = atomic_get(&burst_remaining);
    if (remaining > 0) {
        atomic_dec(&burst_remaining);
        LOG_DBG("⚡ Burst advertisement %d/%d", BURST_COUNT - remaining + 1, BURST_COUNT);
        k_work_schedule(&adv_work, K_MSEC(BURST_INTERVAL_MS));
        return;
    }

    // Schedule next update with adaptive interval
    uint32_t interval_ms = get_current_update_interval();

    // Periodic logging (every 20th update to avoid spam)
    static int update_counter = 0;
    update_counter++;
    if (update_counter % 20 == 0) {
        LOG_INF("📊 PROSPECTOR: %dms intervals (%s) - %s",
                interval_ms, is_active ? "ACTIVE" : "IDLE",
                prospector_adv_active ? (prospector_adv_connectable ? "PROXY_CONN" : "OWN_NC") :
                zmk_adv_was_active ? "PIGGYBACK" : "WAITING");
    }

    k_work_schedule(&adv_work, K_MSEC(interval_ms));
}

// Initialize Prospector simple advertising system
static int init_prospector_status(void) {
    k_work_init_delayable(&adv_work, adv_work_handler);

#if IS_ENABLED(CONFIG_ZMK_STATUS_ADV_ACTIVITY_BASED)
    LOG_INF("⚙️ PROSPECTOR: Activity-based advertisement initialized");
    LOG_INF("   ACTIVE interval: %dms (%.1fHz)", ACTIVE_UPDATE_INTERVAL_MS, 1000.0f/ACTIVE_UPDATE_INTERVAL_MS);
    LOG_INF("   IDLE interval: %dms (%.1fHz)", IDLE_UPDATE_INTERVAL_MS, 1000.0f/IDLE_UPDATE_INTERVAL_MS);
    LOG_INF("   Activity timeout: %dms", ACTIVITY_TIMEOUT_MS);
#else
    LOG_INF("⚙️ PROSPECTOR: Fixed advertisement interval: %dms", CONFIG_ZMK_STATUS_ADV_INTERVAL_MS);
#endif

    // Log WPM configuration
    LOG_INF("📊 WPM: Window=%ds, Multiplier=%dx, Decay=%ds",
            CONFIG_ZMK_STATUS_ADV_WPM_WINDOW_SECONDS,
            WPM_WINDOW_MULTIPLIER,
            WPM_DECAY_TIMEOUT_MS / 1000);

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Prospector: Peripheral device - advertising disabled to preserve split communication");
    LOG_INF("⚠️  To test manufacturer data, use the RIGHT side (Central) firmware!");
    return 0;
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Prospector: Central device - will advertise status for both keyboard sides");
#else
    LOG_INF("Prospector: Standalone device - advertising enabled");
#endif


    // Override BLE device name if CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME is explicitly set.
    // If empty (default), CONFIG_BT_DEVICE_NAME is used as-is.
#if IS_ENABLED(CONFIG_BT_DEVICE_NAME_DYNAMIC)
    if (strlen(CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME) > 0) {
        int name_err = bt_set_name(CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME);
        if (name_err) {
            LOG_WRN("Failed to set BLE device name: %d", name_err);
        } else {
            LOG_INF("BLE device name overridden to: %s", CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME);
        }
    }
#endif

    // Initialize name-in-AD buffer for periodic name broadcast
    {
        const char *adv_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
        if (strlen(adv_name) == 0) {
            adv_name = CONFIG_BT_DEVICE_NAME;
        }
        size_t name_len = MIN(strlen(adv_name), sizeof(name_adv_buffer) - 1);
        memcpy(name_adv_buffer, adv_name, name_len);
        name_adv_buffer[name_len] = '\0';
        name_ad[1].data_len = name_len;
        LOG_INF("Name-in-AD initialized: \"%s\" (%d bytes)", name_adv_buffer, (int)name_len);
    }

    // Initialize activity tracking
    last_activity_time = k_uptime_get_32();
    is_active = true; // Start in active mode

    // Register disconnect callback for clean handoff between own ADV and ZMK
    bt_conn_cb_register(&prospector_conn_callbacks);

    // Start hybrid advertising with initial burst for immediate scanner detection
    adv_started = true;
    atomic_set(&burst_remaining, BURST_COUNT);  // Burst on boot so scanner shows profile immediately
    k_work_schedule(&adv_work, K_SECONDS(1)); // Wait for ZMK BLE to start
    LOG_INF("Prospector: Hybrid mode - piggyback when disconnected, own ADV when connected");

    return 0;
}

// Public API functions for Prospector status system
int zmk_status_advertisement_init(void) {
    LOG_INF("Prospector advertisement API initialized");
    return 0;
}

int zmk_status_advertisement_update(void) {
    if (!adv_started) {
        return 0;
    }

    // Trigger immediate status update
    k_work_cancel_delayable(&adv_work);
    k_work_schedule(&adv_work, K_NO_WAIT);

    return 0;
}

int zmk_status_advertisement_start(void) {
    if (adv_started) {
        k_work_schedule(&adv_work, K_NO_WAIT);
        LOG_INF("Started Prospector status updates");
    }
    return 0;
}

int zmk_status_advertisement_stop(void) {
    if (adv_started) {
        k_work_cancel_delayable(&adv_work);
        if (prospector_adv_active) {
            bt_le_adv_stop();
            prospector_adv_active = false;
        }
        zmk_adv_was_active = false;
        LOG_INF("Stopped Prospector status updates");
    }
    return 0;
}

// Note: Profile changes are detected through periodic updates (200ms/1000ms intervals)
// This provides sufficient responsiveness without needing complex event listeners


// Initialize Prospector system after ZMK BLE is ready
SYS_INIT(init_prospector_status, APPLICATION, 95);

#endif // CONFIG_ZMK_STATUS_ADVERTISEMENT
