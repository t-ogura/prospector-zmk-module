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

// Note: Layer event listeners removed to avoid undefined symbol issues
// Using periodic polling approach for better compatibility

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

// Layer change listener removed - using polling approach for compatibility

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
    snprintf(prospector_adv_data.layer_name, sizeof(prospector_adv_data.layer_name), "L%d", layer);
#endif
    
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

// Ultra-minimal approach: Store compact 6-byte payload like the successful implementation
static uint8_t compact_payload[6];

// Build 6-byte payload as in successful implementation
static void build_compact_payload(void) {
    // Byte 0-1: Manufacturer ID (0xFFFF for local use)
    compact_payload[0] = 0xFF;
    compact_payload[1] = 0xFF;
    
    // Byte 2-3: Service UUID (0xABCD - Prospector identifier)
    compact_payload[2] = 0xAB;
    compact_payload[3] = 0xCD;
    
    // Byte 4: Battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    compact_payload[4] = battery_level;
    
    // Byte 5: Combined layer + status flags
    uint8_t layer = 0;
#if IS_ENABLED(CONFIG_ZMK_KEYMAP)
    layer = zmk_keymap_highest_layer_active();
    if (layer > 15) layer = 15; // Limit to 4 bits
#endif
    
    uint8_t combined = layer & 0x0F; // Lower 4 bits = layer
    
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
    
    compact_payload[5] = combined;
}

// Try to send compact prospector data without interfering with ZMK
static void send_compact_prospector_data(void) {
    build_compact_payload();
    
    // Build minimal advertising data
    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_MANUFACTURER_DATA, compact_payload, sizeof(compact_payload)),
    };
    
    // Use ZMK-compatible parameters
    static const struct bt_le_adv_param param = {
        .id = BT_ID_DEFAULT,
        .options = BT_LE_ADV_OPT_CONNECTABLE,
        .interval_min = BT_GAP_ADV_FAST_INT_MIN_2,
        .interval_max = BT_GAP_ADV_FAST_INT_MAX_2,
    };
    
    // Try to advertise briefly
    int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err == 0) {
        LOG_INF("Compact Prospector data sent: %02X %02X %02X %02X %02X %02X", 
                compact_payload[0], compact_payload[1], compact_payload[2],
                compact_payload[3], compact_payload[4], compact_payload[5]);
        
        // Brief advertisement, then let ZMK resume
        k_sleep(K_MSEC(500));
        bt_le_adv_stop();
    } else if (err != -EALREADY) {
        LOG_ERR("Failed to start compact advertising: %d", err);
    }
}

static void status_update_work_handler(struct k_work *work) {
// Note: Removed peripheral skip logic - let all devices advertise Prospector data

    if (!status_initialized) {
        return;
    }
    
    // Send compact prospector data
    send_compact_prospector_data();
    
    // Schedule next burst with longer interval (30 seconds)
    // This gives enough time for ZMK to work normally, but ensures we get our data out
    k_work_schedule(&status_update_work, K_SECONDS(30));
}

// Initialize Prospector strategic advertising system  
static int init_prospector_status(const struct device *dev) {
    k_work_init_delayable(&status_update_work, status_update_work_handler);
    
    // Initialize manufacturer data with defaults
    build_prospector_data();
    
    LOG_INF("Prospector: Compact 6-byte advertising enabled for all device types");
    
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