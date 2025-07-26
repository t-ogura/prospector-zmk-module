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

static struct k_work_delayable status_update_work;
static bool status_initialized = false;

// Prospector manufacturer data (6 bytes) 
static uint8_t prospector_mfg_data[6] = {0xFF, 0xFF, 0xAB, 0xCD, 0x00, 0x00};

// Strategic timing to work with ZMK
static uint32_t burst_count = 0;

static void build_prospector_data(void) {
    // Build 6-byte manufacturer data payload
    memset(prospector_mfg_data, 0, sizeof(prospector_mfg_data));
    
    // Byte 0-1: Manufacturer ID (0xFFFF for local use)
    prospector_mfg_data[0] = 0xFF;
    prospector_mfg_data[1] = 0xFF;
    
    // Byte 2-3: Service UUID (0xABCD - Prospector identifier)
    prospector_mfg_data[2] = 0xAB;
    prospector_mfg_data[3] = 0xCD;
    
    // Byte 4: Battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    prospector_mfg_data[4] = battery_level;
    
    // Byte 5: Combined layer + device role flags
    uint8_t layer = 0;
    uint8_t combined = 0;
    
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    layer = zmk_keymap_highest_layer_active();
    if (layer > 15) layer = 15; // Limit to 4 bits
#endif
    
    combined = layer & 0x0F; // Lower 4 bits = layer
    
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        combined |= 0x10; // Bit 4 = USB connected
    }
#endif
    
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    combined |= 0x40; // Bit 6 = Central device
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)  
    combined |= 0x80; // Bit 7 = Peripheral device
#endif
    
    prospector_mfg_data[5] = combined;
    
    const char *role_str = 
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        "CENTRAL";
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)
        "PERIPHERAL";
#else
        "STANDALONE";
#endif
    
    LOG_INF("Prospector %s: Battery %d%%, Layer %d", role_str, battery_level, layer);
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
    prospector_burst_data[1].data_len = sizeof(prospector_mfg_data);
    prospector_burst_data[1].data = prospector_mfg_data;
    
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
    if (!status_initialized) {
        return;
    }
    
    // Send strategic advertising to compete with ZMK
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