/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/usb.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/status_advertisement.h>

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

// REVERT TO WORKING APPROACH: Simple separated advertising  
#pragma message "*** PROSPECTOR SIMPLE SEPARATED ADVERTISING ***"

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
static bool default_adv_stopped = false;

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
static int layer_changed_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev && ev->state) { // Only on layer activation
        LOG_DBG("🔄 Layer changed to %d - triggering immediate advertisement update", ev->layer);
        if (adv_started) {
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

// Advertisement packet: Flags + Manufacturer Data ONLY (for 31-byte limit)  
static struct bt_data adv_data_array[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, (uint8_t*)&manufacturer_data, sizeof(manufacturer_data)),
};

// Scan response: Name + Appearance (sent separately)
// Note: Scan response also has 31-byte limit, so we may need to truncate long names
static char device_name_buffer[24]; // Reserve space for name (31 - header bytes)

static struct bt_data scan_rsp[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, device_name_buffer, 0), // Length set dynamically
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03), // HID Keyboard appearance
};

// Custom advertising parameters (connectable only, name in scan response)
static const struct bt_le_adv_param adv_params = {
    .id = BT_ID_DEFAULT,
    .options = BT_LE_ADV_OPT_CONNECTABLE,
    .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
    .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
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
    layer = zmk_keymap_highest_layer_active();
    if (layer > 15) layer = 15;
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

    if (strcmp(central_side, "LEFT") == 0) {
        // Central is on LEFT: 
        // peripheral_battery[0] = RIGHT keyboard (first peripheral)
        // peripheral_battery[1] = AUX (if exists)
        // peripheral_battery[2] = unused
        manufacturer_data.peripheral_battery[0] = peripheral_batteries[0]; // Right keyboard
        manufacturer_data.peripheral_battery[1] = peripheral_batteries[1]; // Aux if exists
        manufacturer_data.peripheral_battery[2] = peripheral_batteries[2]; // Third peripheral
    } else {
        // Central is on RIGHT (default):
        // peripheral_battery[0] = LEFT keyboard (first peripheral)
        // peripheral_battery[1] = AUX (if exists)  
        // peripheral_battery[2] = unused
        memcpy(manufacturer_data.peripheral_battery, peripheral_batteries, 3);
    }
           
#elif IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    // Peripheral: Skip advertising to preserve split communication  
    return;
#else
    manufacturer_data.device_role = ZMK_DEVICE_ROLE_STANDALONE;
    manufacturer_data.device_index = 0;
    memset(manufacturer_data.peripheral_battery, 0, 3);
#endif
    
    // Compact layer name (4 bytes) - reduced from 6
    snprintf(manufacturer_data.layer_name, sizeof(manufacturer_data.layer_name), "L%d", layer);
    
    // Keyboard ID (4 bytes)
    const char *keyboard_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    uint32_t id_hash = 0;
    for (int i = 0; keyboard_name[i] && i < 8; i++) {
        id_hash = id_hash * 31 + keyboard_name[i];
    }
    memcpy(manufacturer_data.keyboard_id, &id_hash, 4);
    
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

// RESTORE v1.1.1 APPROACH: Complete advertising replacement
// This was the ONLY approach that worked with LED + Split communication
static int stop_default_advertising(const struct device *dev) {
    if (default_adv_stopped) {
        return 0;
    }

    LOG_INF("Prospector: Stopping default ZMK advertising (v1.1.1 working approach)");
    int err = bt_le_adv_stop();
    if (err && err != -EALREADY) {
        LOG_ERR("bt_le_adv_stop failed: %d", err);
    } else {
        LOG_INF("Default advertising stopped - this approach worked in v1.1.1");
        default_adv_stopped = true;
    }
    return 0;
}

static void start_custom_advertising(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    // CRITICAL FIX: Don't interfere with peripheral split communication
    LOG_DBG("Skipping advertising on peripheral device to preserve split communication");
    return;
#endif

    if (!default_adv_stopped) {
        LOG_DBG("Default advertising not stopped yet, trying again");
        stop_default_advertising(NULL);
        k_sleep(K_MSEC(50)); // Wait for stop to complete
    }

    build_manufacturer_payload();
    
    // Prepare device name for scan response (respecting 31-byte limit)
    const char *full_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    int full_name_len = strlen(full_name);
    
    // Calculate available space: 31 - (name_header=2) - (appearance_header=1) - (appearance_data=2) = 26
    int max_name_len = sizeof(device_name_buffer) - 1;
    int actual_name_len = MIN(full_name_len, max_name_len);
    
    memcpy(device_name_buffer, full_name, actual_name_len);
    device_name_buffer[actual_name_len] = '\0';
    
    // Update scan response data length
    scan_rsp[0].data_len = actual_name_len;
    
    LOG_DBG("Prospector: Starting separated adv/scan_rsp advertising");
    LOG_DBG("ADV packet: Flags + Manufacturer Data = %d bytes", 3 + 2 + sizeof(manufacturer_data));
    LOG_DBG("SCAN_RSP: Name + Appearance = %d bytes", 2 + actual_name_len + 3);

    // Start advertising with separated adv_data and scan_rsp
    int err = bt_le_adv_start(&adv_params, adv_data_array, ARRAY_SIZE(adv_data_array),
                              scan_rsp, ARRAY_SIZE(scan_rsp));

    if (err == 0) {
        LOG_INF("✅ Advertising started successfully");
    } else if (err == -E2BIG) {
        LOG_ERR("❌ Advertising failed: -E2BIG (payload too large - %d bytes exceeds 31-byte limit)",
                3 + 2 + sizeof(manufacturer_data)); // Flags + ManufData header + payload
    } else {
        LOG_ERR("❌ Advertising failed with error: %d", err);
    }
    
    LOG_INF("Manufacturer data (%d bytes): %02X%02X %02X%02X %02X %02X %02X %02X %02X %02X %02X", 
            sizeof(manufacturer_data),
            manufacturer_data.manufacturer_id[0], manufacturer_data.manufacturer_id[1], 
            manufacturer_data.service_uuid[0], manufacturer_data.service_uuid[1], 
            manufacturer_data.version, manufacturer_data.battery_level, manufacturer_data.active_layer,
            manufacturer_data.profile_slot, manufacturer_data.connection_count, manufacturer_data.status_flags, manufacturer_data.device_role);
    
    // Log the complete data structure in hex for debugging
    LOG_INF("Complete manufacturer data (26 bytes):");
    uint8_t *data_bytes = (uint8_t*)&manufacturer_data;
    for (int i = 0; i < sizeof(manufacturer_data); i += 8) {
        int remaining = sizeof(manufacturer_data) - i;
        if (remaining >= 8) {
            LOG_INF("  [%02d-%02d]: %02X %02X %02X %02X %02X %02X %02X %02X", 
                    i, i+7,
                    data_bytes[i], data_bytes[i+1], data_bytes[i+2], data_bytes[i+3],
                    data_bytes[i+4], data_bytes[i+5], data_bytes[i+6], data_bytes[i+7]);
        } else {
            // Handle last incomplete chunk
            char hex_str[32] = {0};
            int pos = 0;
            for (int j = 0; j < remaining; j++) {
                pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02X ", data_bytes[i + j]);
            }
            LOG_INF("  [%02d-%02d]: %s", i, i + remaining - 1, hex_str);
        }
    }
}

static void adv_work_handler(struct k_work *work) {
    // Update manufacturer data
    build_manufacturer_payload();
    
    // Try to update existing advertising data first
    int err = bt_le_adv_update_data(adv_data_array, ARRAY_SIZE(adv_data_array), 
                                    scan_rsp, ARRAY_SIZE(scan_rsp));
    
    if (err == 0) {
        LOG_INF("✅ Advertising data updated successfully");
    } else {
        // Any error - restart advertising to ensure continuous broadcast
        LOG_INF("Advertising update failed (%d), restarting...", err);
        
        // Stop any existing advertising
        bt_le_adv_stop();
        k_sleep(K_MSEC(50));
        
        // Always try to restart advertising regardless of connection state
        start_custom_advertising();
    }
    
    // Schedule next update with adaptive interval
    uint32_t interval_ms = get_current_update_interval();
    
    // Periodic logging of current interval (every 20th update to avoid spam)
    static int update_counter = 0;
    update_counter++;
    if (update_counter % 20 == 0) {
        LOG_INF("📊 PROSPECTOR: Using %dms intervals (%.1fHz) - %s mode", 
                interval_ms, 1000.0f/interval_ms, is_active ? "ACTIVE" : "IDLE");
    }
    
    k_work_schedule(&adv_work, K_MSEC(interval_ms));
}

// Initialize Prospector simple advertising system  
static int init_prospector_status(const struct device *dev) {
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
    
    // RESTORE WORKING APPROACH: Stop ZMK advertising early
    stop_default_advertising(NULL);
    
    // Initialize activity tracking
    last_activity_time = k_uptime_get_32();
    is_active = true; // Start in active mode
    
    // Start custom advertising - RESTORE WORKING TIMING
    adv_started = true;
    k_work_schedule(&adv_work, K_SECONDS(1)); // Original working timing
    LOG_INF("Prospector: Started custom advertising with original working timing");
    
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
        bt_le_adv_stop();
        LOG_INF("Stopped Prospector status updates");
    }
    return 0;
}

// Note: Profile changes are detected through periodic updates (200ms/1000ms intervals)
// This provides sufficient responsiveness without needing complex event listeners

// Initialize early to stop default advertising before ZMK starts it
SYS_INIT(stop_default_advertising, APPLICATION, 90);

// Initialize Prospector system after ZMK BLE is ready
SYS_INIT(init_prospector_status, APPLICATION, 95);

#endif // CONFIG_ZMK_STATUS_ADVERTISEMENT