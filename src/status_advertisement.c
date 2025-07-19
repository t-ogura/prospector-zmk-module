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
#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/event_manager.h>

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
#include <zmk/behavior.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_STATUS_ADVERTISEMENT)

static struct zmk_status_adv_data adv_data;
static struct k_work_delayable adv_work;
static bool adv_started = false;
static uint32_t current_interval_ms = CONFIG_ZMK_STATUS_ADV_INTERVAL_MS;
static enum zmk_activity_state current_activity_state = ZMK_ACTIVITY_ACTIVE;

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
    
    // Get active layer (only available on central or non-split keyboards)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint8_t layer = zmk_keymap_highest_layer_active();
    adv_data.active_layer = layer;
#else
    // Split peripheral doesn't have access to keymap layer information
    adv_data.active_layer = 0;
#endif
    
    // Get active profile (only available on central or non-split keyboards)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint8_t profile = zmk_ble_active_profile_index();
    adv_data.profile_slot = profile;
#else
    // Split peripheral doesn't have access to BLE profile information
    adv_data.profile_slot = 0;
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
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    snprintf(adv_data.layer_name, sizeof(adv_data.layer_name), "L%d", layer);
#else
    // Split peripheral - use default layer name
    snprintf(adv_data.layer_name, sizeof(adv_data.layer_name), "L0");
#endif
    
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

static void advertisement_work_handler(struct k_work *work) {
    if (!adv_started) {
        return;
    }
    
    update_advertisement_data();
    
    // Create advertisement data
    struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_MANUFACTURER_DATA, &adv_data, sizeof(adv_data)),
    };
    
    // Start advertising
    int err = bt_le_adv_start(BT_LE_ADV_NCONN, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("Failed to start advertising: %d", err);
    }
    
    // Schedule next update with current interval
    k_work_schedule(&adv_work, K_MSEC(current_interval_ms));
}

int zmk_status_advertisement_set_interval(uint32_t interval_ms) {
    if (interval_ms < 100 || interval_ms > 10000) {
        LOG_WRN("Invalid interval %d ms, must be 100-10000", interval_ms);
        return -EINVAL;
    }
    
    current_interval_ms = interval_ms;
    LOG_DBG("Advertisement interval set to %d ms", interval_ms);
    
    // If currently running, reschedule with new interval
    if (adv_started) {
        k_work_cancel_delayable(&adv_work);
        k_work_schedule(&adv_work, K_MSEC(current_interval_ms));
    }
    
    return 0;
}

static void update_activity_based_interval(enum zmk_activity_state state) {
#if IS_ENABLED(CONFIG_ZMK_STATUS_ADV_ACTIVITY_BASED)
    uint32_t new_interval;
    
    switch (state) {
        case ZMK_ACTIVITY_ACTIVE:
            // Active state: faster updates for responsive display
            new_interval = CONFIG_ZMK_STATUS_ADV_ACTIVE_INTERVAL_MS;
            LOG_DBG("Activity ACTIVE: fast advertisement (%d ms)", new_interval);
            break;
            
        case ZMK_ACTIVITY_IDLE:
            // Idle state: slower updates to save power
            new_interval = CONFIG_ZMK_STATUS_ADV_IDLE_INTERVAL_MS;
            LOG_DBG("Activity IDLE: slow advertisement (%d ms)", new_interval);
            break;
            
        case ZMK_ACTIVITY_SLEEP:
#if IS_ENABLED(CONFIG_ZMK_STATUS_ADV_STOP_ON_SLEEP)
            // Sleep state: stop advertisement to allow deep sleep
            LOG_DBG("Activity SLEEP: stopping advertisement");
            zmk_status_advertisement_stop();
            return;
#else
            // Sleep state but keep minimal advertisement
            new_interval = CONFIG_ZMK_STATUS_ADV_IDLE_INTERVAL_MS * 2;
            LOG_DBG("Activity SLEEP: minimal advertisement (%d ms)", new_interval);
#endif
            break;
            
        default:
            new_interval = CONFIG_ZMK_STATUS_ADV_INTERVAL_MS;
            break;
    }
    
    zmk_status_advertisement_set_interval(new_interval);
    
    // Start advertisement if it was stopped and we're not going to sleep
    if (!adv_started && state != ZMK_ACTIVITY_SLEEP) {
        zmk_status_advertisement_start();
    }
#else
    // Activity-based control disabled, use fixed interval
    (void)state; // Suppress unused parameter warning
#endif
}

static int activity_state_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *event = as_zmk_activity_state_changed(eh);
    if (event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    current_activity_state = event->state;
    LOG_DBG("Activity state changed to: %d", event->state);
    
#if IS_ENABLED(CONFIG_ZMK_STATUS_ADVERTISEMENT)
    update_activity_based_interval(event->state);
#endif
    
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_adv_activity, activity_state_listener);
ZMK_SUBSCRIPTION(status_adv_activity, zmk_activity_state_changed);

int zmk_status_advertisement_init(void) {
    k_work_init_delayable(&adv_work, advertisement_work_handler);
    
    // Initialize with current activity state
    current_activity_state = zmk_activity_get_state();
    update_activity_based_interval(current_activity_state);
    
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

// Initialize on system startup
SYS_INIT(zmk_status_advertisement_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif // CONFIG_ZMK_STATUS_ADVERTISEMENT