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

// STRATEGIC APPROACH: Work with ZMK's advertising patterns
#pragma message "*** PROSPECTOR STRATEGIC ADVERTISING ***"

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// No additional includes needed - zmk_peripheral_battery_state_changed is in battery_state_changed.h
#endif

static struct k_work_delayable status_update_work;
static bool status_initialized = false;

// Prospector manufacturer data - now expanded for split keyboard support
static struct zmk_status_adv_data prospector_adv_data = {0};

// Peripheral battery tracking for split keyboards
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static uint8_t peripheral_batteries[3] = {0, 0, 0}; // Up to 3 peripheral devices
static int peripheral_battery_listener(const zmk_event_t *eh);

ZMK_LISTENER(prospector_peripheral_battery, peripheral_battery_listener);
ZMK_SUBSCRIPTION(prospector_peripheral_battery, zmk_peripheral_battery_state_changed);
#endif

// Layer change event listener
static int layer_state_listener(const zmk_event_t *eh);
ZMK_LISTENER(prospector_layer_state, layer_state_listener);
ZMK_SUBSCRIPTION(prospector_layer_state, zmk_layer_state_changed);

// Strategic timing to work with ZMK
static uint32_t burst_count = 0;

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
        if (status_initialized) {
            k_work_cancel_delayable(&status_update_work);
            k_work_schedule(&status_update_work, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
#endif

// Layer change event listener 
static int layer_state_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev) {
        LOG_INF("Layer changed: %d, active: %s", ev->layer, ev->state ? "true" : "false");
        // Trigger immediate status update when layer changes
        if (status_initialized) {
            k_work_cancel_delayable(&status_update_work);
            k_work_schedule(&status_update_work, K_NO_WAIT);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void build_prospector_data(void) {
    // Build complete zmk_status_adv_data structure
    memset(&prospector_adv_data, 0, sizeof(prospector_adv_data));
    
    // Fixed header
    prospector_adv_data.manufacturer_id[0] = 0xFF;
    prospector_adv_data.manufacturer_id[1] = 0xFF;
    prospector_adv_data.service_uuid[0] = 0xAB;
    prospector_adv_data.service_uuid[1] = 0xCD;
    prospector_adv_data.version = ZMK_STATUS_ADV_VERSION;
    
    // Central/Standalone battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    prospector_adv_data.battery_level = battery_level;
    
    // Layer information
#if IS_ENABLED(CONFIG_ZMK_KEYMAP)
    uint8_t layer = zmk_keymap_highest_layer_active();
    if (layer > 15) layer = 15;
    prospector_adv_data.active_layer = layer;
#endif
    
    // Device name from config
    const char *keyboard_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    strncpy(prospector_adv_data.device_name, keyboard_name, sizeof(prospector_adv_data.device_name) - 1);
    prospector_adv_data.device_name[sizeof(prospector_adv_data.device_name) - 1] = '\0';
    
    // Profile and connection info (placeholder for now)
    prospector_adv_data.profile_slot = 0; // TODO: Get actual profile
    prospector_adv_data.connection_count = 1; // TODO: Get actual count
    
    // Status flags
    uint8_t flags = 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        flags |= ZMK_STATUS_FLAG_USB_CONNECTED;
    }
#endif
    // TODO: Add caps word and charging detection
    prospector_adv_data.status_flags = flags;
    
    // Device role
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    prospector_adv_data.device_role = ZMK_DEVICE_ROLE_CENTRAL;
    prospector_adv_data.device_index = 0; // Central is always index 0
    
    // Copy peripheral battery levels
    memcpy(prospector_adv_data.peripheral_battery, peripheral_batteries, 
           sizeof(prospector_adv_data.peripheral_battery));
           
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
    prospector_adv_data.device_role = ZMK_DEVICE_ROLE_PERIPHERAL;
    prospector_adv_data.device_index = 1; // TODO: Get actual peripheral index
    memset(prospector_adv_data.peripheral_battery, 0, sizeof(prospector_adv_data.peripheral_battery));
#else
    prospector_adv_data.device_role = ZMK_DEVICE_ROLE_STANDALONE;
    prospector_adv_data.device_index = 0;
    memset(prospector_adv_data.peripheral_battery, 0, sizeof(prospector_adv_data.peripheral_battery));
#endif
    
    // Keyboard ID (simple hash of device name for now)
    const char *keyboard_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    uint32_t id_hash = 0;
    for (int i = 0; keyboard_name[i] && i < 8; i++) {
        id_hash = id_hash * 31 + keyboard_name[i];
    }
    memcpy(prospector_adv_data.keyboard_id, &id_hash, sizeof(prospector_adv_data.keyboard_id));
    
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
            prospector_adv_data.peripheral_battery[0],
            prospector_adv_data.peripheral_battery[1], 
            prospector_adv_data.peripheral_battery[2],
            prospector_adv_data.active_layer);
}

// Strategic advertising approach
static struct bt_data prospector_burst_data[2];

// Slower, longer advertising to compete with ZMK effectively  
static const struct bt_le_adv_param strategic_params = {
    .id = BT_ID_DEFAULT,
    .options = BT_LE_ADV_OPT_CONNECTABLE, // Make it connectable like ZMK's
    .interval_min = BT_GAP_ADV_SLOW_INT_MIN, // Slower intervals
    .interval_max = BT_GAP_ADV_SLOW_INT_MAX,
};

// STRATEGIC APPROACH: Forcibly take control for longer periods
static void send_prospector_strategic(void) {
    burst_count++;
    
    // Update manufacturer data with current status
    build_prospector_data();
    
    // Build complete advertising packet like ZMK does
    prospector_burst_data[0].type = BT_DATA_FLAGS;
    prospector_burst_data[0].data_len = 1;
    static const uint8_t flags = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
    prospector_burst_data[0].data = &flags;
    
    prospector_burst_data[1].type = BT_DATA_MANUFACTURER_DATA;
    prospector_burst_data[1].data_len = sizeof(prospector_adv_data);
    prospector_burst_data[1].data = (uint8_t *)&prospector_adv_data;
    
    // Stop any existing advertising first
    bt_le_adv_stop();
    k_sleep(K_MSEC(50)); // Wait for stop to complete
    
    // Start our advertising with longer duration (5 seconds)
    int err = bt_le_adv_start(&strategic_params, prospector_burst_data, 2, NULL, 0);
    if (err == 0) {
        // Success - let it run for 5 seconds
        k_sleep(K_SECONDS(5));
        bt_le_adv_stop();
        
        // Give ZMK time to resume its advertising
        k_sleep(K_MSEC(500));
    }
}

static void status_update_work_handler(struct k_work *work) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
    // CRITICAL FIX: Don't interfere with peripheral split communication
    // Peripheral devices need to maintain their advertising for split connection
    LOG_DBG("Skipping advertising on peripheral device to preserve split communication");
    k_work_schedule(&status_update_work, K_SECONDS(30)); // Keep checking
    return;
#endif

    if (!status_initialized) {
        return;
    }
    
    // Send strategic advertising to compete with ZMK (Central side only)
    send_prospector_strategic();
    
    // Schedule next burst with longer interval (30 seconds)
    // This gives enough time for ZMK to work normally, but ensures we get our data out
    k_work_schedule(&status_update_work, K_SECONDS(30));
}

// Initialize Prospector strategic advertising system  
static int init_prospector_status(const struct device *dev) {
    k_work_init_delayable(&status_update_work, status_update_work_handler);
    
    // Initialize manufacturer data with defaults
    build_prospector_data();
    
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
    LOG_INF("Prospector: Peripheral device - advertising disabled to preserve split communication");
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_INF("Prospector: Central device - will advertise status for both keyboard sides");
#else
    LOG_INF("Prospector: Standalone device - advertising enabled");
#endif
    
    // Start strategic advertising after ZMK is fully initialized
    status_initialized = true;
    k_work_schedule(&status_update_work, K_SECONDS(10)); // Wait 10 seconds for ZMK to stabilize
    
    return 0;
}

// Public API functions for Prospector status system
int zmk_status_advertisement_init(void) {
    LOG_INF("Prospector advertisement API initialized");
    return 0;
}

int zmk_status_advertisement_update(void) {
    if (!status_initialized) {
        return 0;
    }
    
    // Trigger immediate status update
    k_work_cancel_delayable(&status_update_work);
    k_work_schedule(&status_update_work, K_NO_WAIT);
    
    return 0;
}

int zmk_status_advertisement_start(void) {
    if (status_initialized) {
        k_work_schedule(&status_update_work, K_NO_WAIT);
        LOG_INF("Started Prospector status updates");
    }
    return 0;
}

int zmk_status_advertisement_stop(void) {
    if (status_initialized) {
        k_work_cancel_delayable(&status_update_work);
        LOG_INF("Stopped Prospector status updates");
    }
    return 0;
}

// Initialize Prospector system after ZMK BLE is ready, but don't interfere with it
SYS_INIT(init_prospector_status, APPLICATION, 95);

#endif // CONFIG_ZMK_STATUS_ADVERTISEMENT