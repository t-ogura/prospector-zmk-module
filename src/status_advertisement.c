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

// Force a compilation error if not enabled properly
#if !IS_ENABLED(CONFIG_ZMK_STATUS_ADVERTISEMENT)
#error "CONFIG_ZMK_STATUS_ADVERTISEMENT is not enabled!"
#endif

// Force a compilation message to verify module is being compiled
#pragma message "*** PROSPECTOR MODULE IS BEING COMPILED ***"

static struct zmk_status_adv_data adv_data;
static struct k_work_delayable adv_work;
static bool adv_started = false;

static void update_advertisement_data(void) {
    // Clear the structure
    memset(&adv_data, 0, sizeof(adv_data));
    
    // Set manufacturer ID (0xFFFF = Reserved for local use)
    adv_data.manufacturer_id[0] = 0xFF;
    adv_data.manufacturer_id[1] = 0xFF;
    
    // Set service UUID
    adv_data.service_uuid[0] = (ZMK_STATUS_ADV_SERVICE_UUID >> 8) & 0xFF;
    adv_data.service_uuid[1] = ZMK_STATUS_ADV_SERVICE_UUID & 0xFF;
    
    // Set protocol version
    adv_data.version = ZMK_STATUS_ADV_VERSION;
    
    // Get battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    adv_data.battery_level = battery_level;
    
    // Get active layer (only available on central/standalone devices)
    uint8_t layer = 0; // Default for peripheral devices
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    layer = zmk_keymap_highest_layer_active();
    adv_data.active_layer = layer;
#else
    // Peripheral devices don't have keymap access
    adv_data.active_layer = 0;
#endif
    
    // Get active profile (only available on central/standalone devices)  
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t profile = zmk_ble_active_profile_index();
    adv_data.profile_slot = profile;
#else
    // Peripheral devices don't have BLE profile access
    adv_data.profile_slot = 0;
#endif
    
    // Set device role and index for split keyboard support
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    adv_data.device_role = ZMK_DEVICE_ROLE_CENTRAL;
    adv_data.device_index = 0; // Central is always index 0
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)  
    adv_data.device_role = ZMK_DEVICE_ROLE_PERIPHERAL;
    adv_data.device_index = 1; // TODO: Get actual peripheral index
#else
    adv_data.device_role = ZMK_DEVICE_ROLE_STANDALONE;
    adv_data.device_index = 0;
#endif
    
    // Get connection count - simplified for now
    // TODO: Implement proper connection counting
    adv_data.connection_count = 1; // Default to 1 for now
    
    // Set status flags
    adv_data.status_flags = 0;
    
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
    // Check caps word status - this function might not exist
    // TODO: Replace with correct caps word check function
    // if (zmk_behavior_caps_word_is_active()) {
    //     adv_data.status_flags |= ZMK_STATUS_FLAG_CAPS_WORD;
    // }
#endif
    
#if IS_ENABLED(CONFIG_ZMK_USB)
    // Check USB connection
    if (zmk_usb_is_powered()) {
        adv_data.status_flags |= ZMK_STATUS_FLAG_USB_CONNECTED;
    }
#endif
    
    // Check charging status - this function might not exist
    // TODO: Replace with correct charging status check
    // if (zmk_battery_is_charging()) {
    //     adv_data.status_flags |= ZMK_STATUS_FLAG_CHARGING;
    // }
    
    // Get layer name - use simple layer number for now
    // TODO: Implement proper layer name lookup
    snprintf(adv_data.layer_name, sizeof(adv_data.layer_name), "L%d", layer);
    
    // Set keyboard ID (derived from device name)
    const char *keyboard_name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    uint32_t hash = 0;
    for (int i = 0; keyboard_name[i] != '\0'; i++) {
        hash = hash * 31 + keyboard_name[i];
    }
    adv_data.keyboard_id[0] = (hash >> 24) & 0xFF;
    adv_data.keyboard_id[1] = (hash >> 16) & 0xFF;
    adv_data.keyboard_id[2] = (hash >> 8) & 0xFF;
    adv_data.keyboard_id[3] = hash & 0xFF;
}

// BLE advertising modification approach
#include <zephyr/bluetooth/bluetooth.h>

// External reference to ZMK's advertising data (declared in zmk/app/src/ble.c)
extern struct bt_data zmk_ble_ad[];

// Storage for our extended advertising data
static struct bt_data zmk_extended_ad[4]; // Original 3 + our manufacturer data
static bool ad_extended = false;

static void extend_zmk_advertising(void) {
    if (ad_extended) {
        return; // Already extended
    }
    
    // Copy original ZMK advertising data
    zmk_extended_ad[0] = zmk_ble_ad[0]; // GAP Appearance
    zmk_extended_ad[1] = zmk_ble_ad[1]; // Flags
    zmk_extended_ad[2] = zmk_ble_ad[2]; // Service UUIDs
    
    // Add our manufacturer data
    zmk_extended_ad[3] = (struct bt_data){
        .type = BT_DATA_MANUFACTURER_DATA,
        .data_len = sizeof(adv_data),
        .data = (uint8_t*)&adv_data
    };
    
    // Replace ZMK's advertising data with our extended version
    memcpy(zmk_ble_ad, zmk_extended_ad, sizeof(zmk_extended_ad));
    
    ad_extended = true;
    printk("*** PROSPECTOR: Extended ZMK advertising with manufacturer data ***\n");
}

static void advertisement_work_handler(struct k_work *work) {
    if (!adv_started) {
        printk("*** PROSPECTOR: Advertisement work called but not started ***\n");
        LOG_WRN("Advertisement work called but not started");
        return;
    }
    
    printk("*** PROSPECTOR: Updating advertisement data ***\n");
    LOG_DBG("Updating advertisement data");
    update_advertisement_data();
    
    // Extend ZMK's advertising to include our manufacturer data
    extend_zmk_advertising();
    
    printk("*** PROSPECTOR: Updated manufacturer data - %s, battery: %d%%, layer: %d ***\n", 
            CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME, 
            adv_data.battery_level, 
            adv_data.active_layer);
    
    printk("*** PROSPECTOR: Manufacturer data: %02X %02X %02X %02X %02X %02X %02X %02X ***\n",
            adv_data.manufacturer_id[0], adv_data.manufacturer_id[1],
            adv_data.service_uuid[0], adv_data.service_uuid[1],
            adv_data.version, adv_data.battery_level,
            adv_data.active_layer, adv_data.profile_slot);
    
    // ZMK will use our extended advertising data automatically
    
    // Schedule next update
    k_work_schedule(&adv_work, K_MSEC(CONFIG_ZMK_STATUS_ADV_INTERVAL_MS));
}

int zmk_status_advertisement_init(void) {
    printk("\n\n*** PROSPECTOR ADVERTISEMENT INIT STARTING ***\n\n");
    printk("*** PROSPECTOR: Status advertisement module loading... ***\n");
    LOG_INF("Status advertisement module loading...");
    
    // Flash LED or some visible indication if available
#ifdef CONFIG_LED
    // Try to flash an LED if available
#endif
    
    k_work_init_delayable(&adv_work, advertisement_work_handler);
    
    // Start advertisement automatically after initialization
    adv_started = true;
    k_work_schedule(&adv_work, K_SECONDS(2)); // Start after 2 seconds to ensure BT is ready
    
    printk("*** PROSPECTOR: Status advertisement initialized and auto-started ***\n");
    printk("*** PROSPECTOR: Advertisement interval: %d ms ***\n", CONFIG_ZMK_STATUS_ADV_INTERVAL_MS);
    printk("*** PROSPECTOR: Keyboard name: %s ***\n", CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME);
    printk("\n\n*** PROSPECTOR ADVERTISEMENT INIT COMPLETE ***\n\n");
    
    LOG_INF("Status advertisement initialized and auto-started");
    LOG_INF("Advertisement interval: %d ms", CONFIG_ZMK_STATUS_ADV_INTERVAL_MS);
    LOG_INF("Keyboard name: %s", CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME);
    
    return 0;
}

int zmk_status_advertisement_update(void) {
    if (!adv_started) {
        return 0;
    }
    
    // Cancel current work and reschedule immediately
    k_work_cancel_delayable(&adv_work);
    k_work_schedule(&adv_work, K_NO_WAIT);
    
    return 0;
}

int zmk_status_advertisement_start(void) {
    if (adv_started) {
        return 0;
    }
    
    adv_started = true;
    k_work_schedule(&adv_work, K_NO_WAIT);
    
    LOG_INF("Started status advertisement broadcasting");
    return 0;
}

int zmk_status_advertisement_stop(void) {
    if (!adv_started) {
        return 0;
    }
    
    adv_started = false;
    k_work_cancel_delayable(&adv_work);
    
    // Stop advertising
    int err = bt_le_adv_stop();
    if (err) {
        LOG_ERR("Failed to stop advertising: %d", err);
    }
    
    LOG_INF("Stopped status advertisement broadcasting");
    return 0;
}

// Initialize on system startup - use later priority to ensure BT is ready
SYS_INIT(zmk_status_advertisement_init, APPLICATION, 99);

#endif // CONFIG_ZMK_STATUS_ADVERTISEMENT