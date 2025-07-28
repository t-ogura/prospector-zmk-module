/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdio.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/usb.h>
#include <zmk/endpoints.h>
#include <zmk/status_advertisement.h>

// keymap API is available on Central or Standalone (non-Split) builds only
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/keymap.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
#include <zmk/behavior.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_STATUS_ADVERTISEMENT)

// REVERT TO WORKING APPROACH: Simple separated advertising  
#pragma message "*** PROSPECTOR SIMPLE SEPARATED ADVERTISING ***"

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// No additional includes needed - zmk_peripheral_battery_state_changed is in battery_state_changed.h
#endif

static struct zmk_status_adv_data adv_data;
static struct k_work_delayable adv_work;
static bool adv_started = false;
static bool default_adv_stopped = false;

// Adaptive update intervals based on activity - faster for layer detection
#define ACTIVE_UPDATE_INTERVAL_MS    500   // 2Hz when active (key presses)
#define IDLE_UPDATE_INTERVAL_MS     1000   // 1Hz when idle (faster for layer detection)
#define ACTIVITY_TIMEOUT_MS        10000   // 10 seconds to consider idle

static uint32_t last_activity_time = 0;
static bool is_active = false;

// Latest layer state for accurate tracking
static uint8_t latest_layer = 0;

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
        
        LOG_INF("🔥 Key activity detected - switching to high frequency updates");
        
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

// Helper function to determine current update interval
static uint32_t get_current_update_interval(void) {
    uint32_t now = k_uptime_get_32();
    
    // Check if we should transition from active to idle
    if (is_active && (now - last_activity_time) > ACTIVITY_TIMEOUT_MS) {
        is_active = false;
        LOG_INF("💤 Switching to idle mode - reducing update frequency");
    }
    
    uint32_t interval = is_active ? ACTIVE_UPDATE_INTERVAL_MS : IDLE_UPDATE_INTERVAL_MS;
    LOG_DBG("Update interval: %dms (%s mode)", interval, is_active ? "ACTIVE" : "IDLE");
    return interval;
}


// BLE Legacy Advertising 31-byte limit: Flags(3) + Manufacturer(2+N) <= 31
// Therefore: max manufacturer payload = 31 - 3 - 2 = 26 bytes
#define MAX_ADV_DATA_LEN     31
#define FLAGS_LEN           3   // 1 length + 1 type + 1 data  
#define MANUF_OVERHEAD      2   // 1 length + 1 type
#define MAX_MANUF_PAYLOAD   (MAX_ADV_DATA_LEN - FLAGS_LEN - MANUF_OVERHEAD) // = 26

static uint8_t manufacturer_data[MAX_MANUF_PAYLOAD]; // 26 bytes maximum

// Advertisement packet: Flags + Manufacturer Data ONLY (for 31-byte limit)  
static struct bt_data adv_data_array[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, manufacturer_data, sizeof(manufacturer_data)),
};

// Scan response: Name + Appearance (sent separately)
static struct bt_data scan_rsp[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME, 
            strlen(CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME)),
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03), // HID Keyboard appearance
};

// Custom advertising parameters (connectable only, no USE_NAME flag)
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
    // Build compact 26-byte manufacturer data structure
    memset(manufacturer_data, 0, sizeof(manufacturer_data));
    
    // Manufacturer ID (first 2 bytes)
    manufacturer_data[0] = 0xFF;
    manufacturer_data[1] = 0xFF;
    
    // Service UUID
    manufacturer_data[2] = 0xAB;
    manufacturer_data[3] = 0xCD;
    manufacturer_data[4] = ZMK_STATUS_ADV_VERSION; // version
    
    // Central/Standalone battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    manufacturer_data[5] = battery_level;
    
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
    
    manufacturer_data[6] = layer; // active_layer
    
    // Embed debug info in reserved bytes for troubleshooting
    // This allows us to see the actual values in BLE scanner apps
    manufacturer_data[23] = layer;  // Current layer (debug copy)
    manufacturer_data[24] = latest_layer;  // Latest tracked layer  
    manufacturer_data[25] = 0x42;  // Debug marker
    
    // Profile and connection info (placeholder for now)
    manufacturer_data[7] = 0; // profile_slot
    manufacturer_data[8] = 1; // connection_count
    
    // Status flags
    uint8_t flags = 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        flags |= ZMK_STATUS_FLAG_USB_CONNECTED;
    }
#endif
    manufacturer_data[9] = flags; // status_flags
    
    // Device role
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    manufacturer_data[10] = ZMK_DEVICE_ROLE_CENTRAL; // device_role
    manufacturer_data[11] = 0; // device_index (Central is always index 0)
    
    // Copy peripheral battery levels (3 bytes at offset 12)
    memcpy(&manufacturer_data[12], peripheral_batteries, 3);
           
#elif IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    // Peripheral: Skip advertising to preserve split communication  
    return;
#else
    manufacturer_data[10] = ZMK_DEVICE_ROLE_STANDALONE; // device_role
    manufacturer_data[11] = 0; // device_index
    memset(&manufacturer_data[12], 0, 3); // peripheral_battery
#endif
    
    // Compact layer name (4 bytes starting at offset 15) - reduced from 6
    snprintf((char*)&manufacturer_data[15], 4, "L%d", layer);
    
    // Keyboard ID (4 bytes at offset 19) - moved forward
    const char *keyboard_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    uint32_t id_hash = 0;
    for (int i = 0; keyboard_name[i] && i < 8; i++) {
        id_hash = id_hash * 31 + keyboard_name[i];
    }
    memcpy(&manufacturer_data[19], &id_hash, 4);
    
    // Reserved bytes (23-25) for future expansion - reduced from 6 to 3
    memset(&manufacturer_data[23], 0, 3);
    
    const char *role_str = 
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        "CENTRAL";
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
        "PERIPHERAL";
#else
        "STANDALONE";
#endif
    
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Prospector %s: Central %d%%, Peripheral [%d,%d,%d], Layer %d", 
            role_str, battery_level, 
            peripheral_batteries[0], peripheral_batteries[1], peripheral_batteries[2],
            layer);
#else
    LOG_INF("Prospector %s: Battery %d%%, Layer %d", 
            role_str, battery_level, layer);
#endif
}

// Stop default advertising early in boot process
static int stop_default_advertising(const struct device *dev) {
    if (default_adv_stopped) {
        return 0;
    }
    
    LOG_INF("Prospector: Stopping default ZMK advertising");
    int err = bt_le_adv_stop();
    if (err && err != -EALREADY) {
        LOG_ERR("bt_le_adv_stop failed: %d", err);
    } else {
        LOG_INF("Default advertising stopped");
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
        LOG_INF("Default advertising not stopped yet, trying again");
        stop_default_advertising(NULL);
        k_sleep(K_MSEC(50)); // Wait for stop to complete
    }
    
    build_manufacturer_payload();
    
    LOG_INF("Prospector: Starting separated adv/scan_rsp advertising");
    LOG_INF("ADV packet: Flags + Manufacturer Data");
    LOG_INF("SCAN_RSP: Name + Appearance");
    
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
            manufacturer_data[0], manufacturer_data[1], 
            manufacturer_data[2], manufacturer_data[3], 
            manufacturer_data[4], manufacturer_data[5], manufacturer_data[6],
            manufacturer_data[7], manufacturer_data[8], manufacturer_data[9], manufacturer_data[10]);
    
    // Log the complete data structure in hex for debugging
    LOG_INF("Complete manufacturer data (26 bytes):");
    for (int i = 0; i < sizeof(manufacturer_data); i += 8) {
        int remaining = sizeof(manufacturer_data) - i;
        if (remaining >= 8) {
            LOG_INF("  [%02d-%02d]: %02X %02X %02X %02X %02X %02X %02X %02X", 
                    i, i+7,
                    manufacturer_data[i], manufacturer_data[i+1], manufacturer_data[i+2], manufacturer_data[i+3],
                    manufacturer_data[i+4], manufacturer_data[i+5], manufacturer_data[i+6], manufacturer_data[i+7]);
        } else {
            // Handle last incomplete chunk
            char hex_str[32] = {0};
            int pos = 0;
            for (int j = 0; j < remaining; j++) {
                pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02X ", manufacturer_data[i + j]);
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
    } else if (err == -EALREADY || err == -EINVAL) {
        // Advertising not started yet, or needs restart - start fresh
        LOG_INF("Advertising not active, starting fresh...");
        start_custom_advertising();
    } else {
        LOG_ERR("❌ Failed to update advertising data: %d", err);
        // Try to restart advertising as fallback
        bt_le_adv_stop();
        k_sleep(K_MSEC(100));
        start_custom_advertising();
    }
    
    // Schedule next update with adaptive interval
    uint32_t interval_ms = get_current_update_interval();
    k_work_schedule(&adv_work, K_MSEC(interval_ms));
}

// Initialize Prospector simple advertising system  
static int init_prospector_status(const struct device *dev) {
    k_work_init_delayable(&adv_work, adv_work_handler);
    
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Prospector: Peripheral device - advertising disabled to preserve split communication");
    LOG_INF("⚠️  To test manufacturer data, use the RIGHT side (Central) firmware!");
    return 0;
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Prospector: Central device - will advertise status for both keyboard sides");
#else
    LOG_INF("Prospector: Standalone device - advertising enabled");
#endif
    
    // Stop ZMK advertising early
    stop_default_advertising(NULL);
    
    // Initialize activity tracking
    last_activity_time = k_uptime_get_32();
    is_active = true; // Start in active mode
    
    // Start custom advertising after a delay
    adv_started = true;
    k_work_schedule(&adv_work, K_SECONDS(5)); // Wait 5 seconds for ZMK to stabilize
    
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

// Initialize early to stop default advertising before ZMK starts it
SYS_INIT(stop_default_advertising, APPLICATION, 90);

// Initialize Prospector system after ZMK BLE is ready
SYS_INIT(init_prospector_status, APPLICATION, 95);

#endif // CONFIG_ZMK_STATUS_ADVERTISEMENT