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
    
    // Profile and connection info (placeholder for now)
    manufacturer_data.profile_slot = 0; // TODO: Get actual BLE profile
    manufacturer_data.connection_count = 1; // TODO: Get actual connection count
    
    // Status flags
    uint8_t flags = 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        flags |= ZMK_STATUS_FLAG_USB_CONNECTED;
    }
#endif
    manufacturer_data.status_flags = flags;
    
    // Device role and peripheral batteries
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    manufacturer_data.device_role = ZMK_DEVICE_ROLE_CENTRAL;
    manufacturer_data.device_index = 0; // Central is always index 0
    
    // Copy peripheral battery levels
    memcpy(manufacturer_data.peripheral_battery, peripheral_batteries, 3);
           
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
    
    // Reserved bytes (3 bytes) for future expansion
    memset(manufacturer_data.reserved, 0, 3);
    
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
    
    // Prepare device name for scan response (respecting 31-byte limit)
    const char *full_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    int full_name_len = strlen(full_name);
    
    // Debug: Show what name we're trying to send and compare with ZMK standard name
    LOG_INF("📝 CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME: '%s' (len=%d)", full_name, full_name_len);
    LOG_INF("📝 CONFIG_BT_DEVICE_NAME: '%s' (len=%d)", CONFIG_BT_DEVICE_NAME, strlen(CONFIG_BT_DEVICE_NAME));
    
    // Calculate available space: 31 - (name_header=2) - (appearance_header=1) - (appearance_data=2) = 26
    int max_name_len = sizeof(device_name_buffer) - 1;
    int actual_name_len = MIN(full_name_len, max_name_len);
    
    LOG_INF("📏 Name length: requested=%d, max_allowed=%d, actual=%d", 
            full_name_len, max_name_len, actual_name_len);
    
    memcpy(device_name_buffer, full_name, actual_name_len);
    device_name_buffer[actual_name_len] = '\0';
    
    // Update scan response data length
    scan_rsp[0].data_len = actual_name_len;
    
    LOG_INF("📤 Scan response will send: '%s' (len=%d)", device_name_buffer, actual_name_len);
    
    LOG_INF("Prospector: Starting separated adv/scan_rsp advertising");
    LOG_INF("ADV packet: Flags + Manufacturer Data = %d bytes", 3 + 2 + sizeof(manufacturer_data));
    LOG_INF("SCAN_RSP: Name + Appearance = %d bytes", 2 + actual_name_len + 3);
    
    // This approach keeps manufacturer_data at full 26 bytes while device name in scan response
    LOG_INF("✅ Advertisement stays within 31-byte limit: %d bytes", 3 + 2 + sizeof(manufacturer_data));
    
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
    k_work_schedule(&adv_work, K_SECONDS(1)); // Reduced delay to minimize "LalaPad" duration
    
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