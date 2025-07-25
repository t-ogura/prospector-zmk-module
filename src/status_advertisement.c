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
static bool default_adv_stopped = false;

// Compact payload for 31-byte BLE limit
static uint8_t compact_payload[8]; // Keep it small for BLE limits

static struct bt_data custom_adv_data[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME, 
            strlen(CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME)),
    BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xC1, 0x03), // HID Keyboard appearance
    BT_DATA(BT_DATA_MANUFACTURER_DATA, compact_payload, sizeof(compact_payload)),
};

static void build_compact_payload(void) {
    // Create compact 8-byte payload for BLE 31-byte limit
    memset(compact_payload, 0, sizeof(compact_payload));
    
    // Byte 0-1: Manufacturer ID (0xFFFF)
    compact_payload[0] = 0xFF;
    compact_payload[1] = 0xFF;
    
    // Byte 2-3: Service UUID (0xABCD)
    compact_payload[2] = 0xAB;
    compact_payload[3] = 0xCD;
    
    // Byte 4: Protocol version
    compact_payload[4] = ZMK_STATUS_ADV_VERSION;
    
    // Byte 5: Battery level
    uint8_t battery_level = zmk_battery_state_of_charge();
    if (battery_level > 100) {
        battery_level = 100;
    }
    compact_payload[5] = battery_level;
    
    // Byte 6: Active layer (only for central/standalone)
    uint8_t layer = 0;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    layer = zmk_keymap_highest_layer_active();
#endif
    compact_payload[6] = layer;
    
    // Byte 7: Status flags (USB connection, device role, etc.)
    uint8_t status = 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        status |= 0x01; // USB connected bit
    }
#endif
    
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    status |= 0x10; // Central device bit
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_PERIPHERAL)  
    status |= 0x20; // Peripheral device bit
#else
    status |= 0x00; // Standalone device (default)
#endif
    
    compact_payload[7] = status;
}

// Stop default advertising early in boot process
static int stop_default_advertising(const struct device *dev) {
    if (default_adv_stopped) {
        return 0;
    }
    
    printk("*** PROSPECTOR: Stopping default ZMK advertising ***\n");
    int err = bt_le_adv_stop();
    if (err && err != -EALREADY) {
        printk("*** PROSPECTOR: bt_le_adv_stop failed: %d ***\n", err);
        LOG_ERR("bt_le_adv_stop failed: %d", err);
    } else {
        printk("*** PROSPECTOR: Default advertising stopped successfully ***\n");
        LOG_INF("Default advertising stopped");
        default_adv_stopped = true;
    }
    return 0;
}

static void start_custom_advertising(void) {
    if (!default_adv_stopped) {
        printk("*** PROSPECTOR: Default advertising not stopped yet, trying again ***\n");
        stop_default_advertising(NULL);
        k_sleep(K_MSEC(50)); // Wait for stop to complete
    }
    
    build_compact_payload();
    
    printk("*** PROSPECTOR: Starting custom advertising ***\n");
    int err = bt_le_adv_start(BT_LE_ADV_CONN_NAME, custom_adv_data, ARRAY_SIZE(custom_adv_data), NULL, 0);
    
    printk("*** PROSPECTOR: Custom advertising result: %d ***\n", err);
    printk("*** PROSPECTOR: Compact payload: %02X %02X %02X %02X %02X %02X %02X %02X ***\n",
            compact_payload[0], compact_payload[1], compact_payload[2], compact_payload[3],
            compact_payload[4], compact_payload[5], compact_payload[6], compact_payload[7]);
    
    if (err == -EALREADY) {
        printk("*** PROSPECTOR: Advertising already active (expected) ***\n");
    } else if (err) {
        printk("*** PROSPECTOR: Custom advertising failed: %d ***\n", err);
        LOG_ERR("Custom advertising failed: %d", err);
    } else {
        printk("*** PROSPECTOR: Custom advertising started successfully ***\n");
        LOG_INF("Custom advertising with manufacturer data started");
    }
}

static void advertisement_work_handler(struct k_work *work) {
    if (!adv_started) {
        return;
    }
    
    // Update and restart advertising
    start_custom_advertising();
    
    // Schedule next update
    k_work_schedule(&adv_work, K_MSEC(CONFIG_ZMK_STATUS_ADV_INTERVAL_MS));
}

// Initialize and start custom advertising after ZMK BLE setup
static int start_custom_adv_system(const struct device *dev) {
    printk("*** PROSPECTOR: Starting custom advertising system ***\n");
    LOG_INF("Starting custom advertising system");
    
    k_work_init_delayable(&adv_work, advertisement_work_handler);
    
    // Start advertisement after brief delay to ensure BT is ready
    adv_started = true;
    k_work_schedule(&adv_work, K_SECONDS(1));
    
    printk("*** PROSPECTOR: Custom advertising system scheduled to start ***\n");
    printk("*** PROSPECTOR: Keyboard name: %s ***\n", CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME);
    printk("*** PROSPECTOR: Advertisement interval: %d ms ***\n", CONFIG_ZMK_STATUS_ADV_INTERVAL_MS);
    
    return 0;
}

int zmk_status_advertisement_init(void) {
    printk("\n\n*** PROSPECTOR ADVERTISEMENT INIT ***\n\n");
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

// Stop default advertising early
SYS_INIT(stop_default_advertising, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

// Start custom advertising system after ZMK BLE setup
SYS_INIT(start_custom_adv_system, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT + 1);

#endif // CONFIG_ZMK_STATUS_ADVERTISEMENT