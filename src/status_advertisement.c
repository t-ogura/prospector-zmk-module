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
#include <zmk/keymap.h>
#include <zmk/ble.h>
#include <zmk/usb.h>
#include <zmk/endpoints.h>
#include <zmk/status_advertisement.h>

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
#include <zmk/behavior.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_STATUS_ADVERTISEMENT)

// REVERT TO WORKING APPROACH: Simple separated advertising  
#pragma message "*** PROSPECTOR SIMPLE SEPARATED ADVERTISING ***"

#include <zmk/events/battery_state_changed.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// No additional includes needed - zmk_peripheral_battery_state_changed is in battery_state_changed.h
#endif

static struct zmk_status_adv_data adv_data;
static struct k_work_delayable adv_work;
static bool adv_started = false;
static bool default_adv_stopped = false;

// Peripheral battery tracking for split keyboards
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static uint8_t peripheral_batteries[3] = {0, 0, 0}; // Up to 3 peripheral devices
static int peripheral_battery_listener(const zmk_event_t *eh);

ZMK_LISTENER(prospector_peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(prospector_peripheral_battery, zmk_peripheral_battery_state_changed);
#endif


// Full manufacturer data including the required 0xFF 0xFF prefix
static uint8_t full_manufacturer_data[31]; // Complete manufacturer data

// Advertisement packet: Flags + Manufacturer Data ONLY (for 31-byte limit)
static struct bt_data adv_data_array[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, full_manufacturer_data, sizeof(full_manufacturer_data)),
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
    // Build complete zmk_status_adv_data structure in full_manufacturer_data
    memset(full_manufacturer_data, 0, sizeof(full_manufacturer_data));
    
    // Manufacturer ID (first 2 bytes)
    full_manufacturer_data[0] = 0xFF;
    full_manufacturer_data[1] = 0xFF;
    
    // Service UUID
    full_manufacturer_data[2] = 0xAB;
    full_manufacturer_data[3] = 0xCD;
    full_manufacturer_data[4] = ZMK_STATUS_ADV_VERSION; // version
    
    // Central/Standalone battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    full_manufacturer_data[5] = battery_level;
    
    // Layer information
    uint8_t layer = 0;
#if IS_ENABLED(CONFIG_ZMK_KEYMAP)
    layer = zmk_keymap_highest_layer_active();
    if (layer > 15) layer = 15;
    full_manufacturer_data[6] = layer; // active_layer
#endif
    
    // Profile and connection info (placeholder for now)
    full_manufacturer_data[7] = 0; // profile_slot
    full_manufacturer_data[8] = 1; // connection_count
    
    // Status flags
    uint8_t flags = 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        flags |= ZMK_STATUS_FLAG_USB_CONNECTED;
    }
#endif
    full_manufacturer_data[9] = flags; // status_flags
    
    // Device role
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    full_manufacturer_data[10] = ZMK_DEVICE_ROLE_CENTRAL; // device_role
    full_manufacturer_data[11] = 0; // device_index (Central is always index 0)
    
    // Copy peripheral battery levels (3 bytes at offset 12)
    memcpy(&full_manufacturer_data[12], peripheral_batteries, 3);
           
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
    // Skip advertising on peripheral to preserve split communication
    return;
#else
    full_manufacturer_data[10] = ZMK_DEVICE_ROLE_STANDALONE; // device_role
    full_manufacturer_data[11] = 0; // device_index
    memset(&full_manufacturer_data[12], 0, 3); // peripheral_battery
#endif
    
    // Layer name (6 bytes starting at offset 15)
    snprintf((char*)&full_manufacturer_data[15], 6, "L%d", layer);
    
    // Keyboard ID (4 bytes at offset 21)
    const char *keyboard_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    uint32_t id_hash = 0;
    for (int i = 0; keyboard_name[i] && i < 8; i++) {
        id_hash = id_hash * 31 + keyboard_name[i];
    }
    memcpy(&full_manufacturer_data[21], &id_hash, 4);
    
    // Reserved bytes (25-30) for future expansion
    memset(&full_manufacturer_data[25], 0, 6);
    
    const char *role_str = 
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        "CENTRAL";
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
        "PERIPHERAL";
#else
        "STANDALONE";
#endif
    
    LOG_INF("Prospector %s: Central %d%%, Peripheral [%d,%d,%d], Layer %d", 
            role_str, battery_level, 
            peripheral_batteries[0], peripheral_batteries[1], peripheral_batteries[2],
            layer);
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
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
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
    
    LOG_INF("Custom advertising result: %d", err);
    LOG_INF("Manufacturer data: %02X%02X %02X%02X %02X %02X %02X", 
            full_manufacturer_data[0], full_manufacturer_data[1], 
            full_manufacturer_data[2], full_manufacturer_data[3], 
            full_manufacturer_data[4], full_manufacturer_data[5], full_manufacturer_data[6]);
}

static void adv_work_handler(struct k_work *work) {
    // Update and restart advertising
    start_custom_advertising();
    
    // Schedule next update
    k_work_schedule(&adv_work, K_SECONDS(CONFIG_ZMK_STATUS_ADV_INTERVAL_MS / 1000));
}

// Initialize Prospector simple advertising system  
static int init_prospector_status(const struct device *dev) {
    k_work_init_delayable(&adv_work, adv_work_handler);
    
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
    LOG_INF("Prospector: Peripheral device - advertising disabled to preserve split communication");
    return 0;
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Prospector: Central device - will advertise status for both keyboard sides");
#else
    LOG_INF("Prospector: Standalone device - advertising enabled");
#endif
    
    // Stop ZMK advertising early
    stop_default_advertising(NULL);
    
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