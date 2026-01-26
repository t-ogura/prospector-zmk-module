/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file periodic_adv_protocol.c
 * @brief Prospector Periodic Advertising Protocol v2.2.0 Implementation
 *
 * Implements the dynamic/static packet system for BLE Periodic Advertising.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>
#include <string.h>

#include <zmk/periodic_adv_protocol.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/hid.h>
#include <zmk/activity.h>

#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
#include <zmk/ble.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
#include <zmk/keymap.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_STATUS_ADV_PERIODIC)

/*============================================================================
 * Configuration
 *============================================================================*/

#define DYNAMIC_INTERVAL_MS  CONFIG_PROSPECTOR_DYNAMIC_PACKET_INTERVAL_MS
#define STATIC_INTERVAL_MS   CONFIG_PROSPECTOR_STATIC_PACKET_INTERVAL_MS

/* Convert ms to BLE interval units (1.25ms) */
#define MS_TO_INTERVAL(ms)   ((ms) * 4 / 5)

/*============================================================================
 * State
 *============================================================================*/

static struct bt_le_ext_adv *per_adv_set = NULL;
static bool per_adv_started = false;
static bool static_update_requested = false;

/* Sequence number for packet ordering */
static uint16_t sequence_number = 0;

/* Last activity time for idle calculation */
static uint32_t last_input_time = 0;

/* Pointer movement accumulator (reset after each packet) */
static int16_t accumulated_dx = 0;
static int16_t accumulated_dy = 0;
static int8_t accumulated_scroll_v = 0;
static int8_t accumulated_scroll_h = 0;
static uint8_t current_pointer_buttons = 0;

/* Cached peripheral battery values */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
extern uint8_t peripheral_batteries[3];  /* Defined in status_advertisement.c */
#endif

/* Cached RSSI values (updated every static packet) */
static int8_t cached_rssi[3] = {RSSI_INVALID, RSSI_INVALID, RSSI_INVALID};

/* Total keypress counter */
static uint32_t total_keypress_count = 0;

/* Boot count (placeholder - could be stored in NVS) */
static uint16_t boot_count = 0;

/* Work items for periodic updates */
static struct k_work_delayable dynamic_work;
static struct k_work_delayable static_work;

/* Current packet data */
static struct periodic_dynamic_packet dynamic_packet;
static struct periodic_static_packet static_packet;

/* External WPM variable from status_advertisement.c */
extern uint8_t current_wpm;

/*============================================================================
 * Helper Functions
 *============================================================================*/

static uint8_t get_device_role(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return 1;  /* CENTRAL */
#elif IS_ENABLED(CONFIG_ZMK_SPLIT)
    return 2;  /* PERIPHERAL */
#else
    return 0;  /* STANDALONE */
#endif
}

static uint32_t generate_keyboard_id(void) {
    const char *name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    uint32_t hash = 0;
    for (int i = 0; name[i] && i < 16; i++) {
        hash = hash * 31 + name[i];
    }
    return hash;
}

static uint8_t get_layer_count(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    /* Count actual layers by checking if layer name exists */
    uint8_t count = 0;
    for (int i = 0; i < 32; i++) {
        const char *name = zmk_keymap_layer_name(i);
        if (name && name[0] != '\0') {
            count = i + 1;
        }
    }
    return count > 0 ? count : 1;
#else
    return 1;
#endif
}

static uint8_t get_device_features(void) {
    uint8_t features = 0;

#if IS_ENABLED(CONFIG_ZMK_MOUSE) || IS_ENABLED(CONFIG_ZMK_POINTING)
    features |= DEVICE_FEATURE_TRACKBALL;
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    features |= DEVICE_FEATURE_RGB;
#endif

#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
    features |= DEVICE_FEATURE_BACKLIGHT;
#endif

#if IS_ENABLED(CONFIG_ENCODER)
    features |= DEVICE_FEATURE_ENCODER;
#endif

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)
    features |= DEVICE_FEATURE_DISPLAY;
#endif

    return features;
}

static void update_peripheral_rssi(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* TODO: Implement RSSI retrieval via bt_conn_le_get_rssi() */
    /* For now, use placeholder values */
    cached_rssi[0] = RSSI_INVALID;
    cached_rssi[1] = RSSI_INVALID;
    cached_rssi[2] = RSSI_INVALID;
#else
    cached_rssi[0] = RSSI_INVALID;
    cached_rssi[1] = RSSI_INVALID;
    cached_rssi[2] = RSSI_INVALID;
#endif
}

/*============================================================================
 * Packet Building
 *============================================================================*/

int periodic_adv_build_dynamic_packet(struct periodic_dynamic_packet *packet) {
    if (!packet) {
        return -EINVAL;
    }

    memset(packet, 0, sizeof(*packet));

    /* Prospector signature (v2.2.0) */
    packet->manufacturer_id[0] = PROSPECTOR_SIGNATURE_0;
    packet->manufacturer_id[1] = PROSPECTOR_SIGNATURE_1;
    packet->service_uuid[0] = PROSPECTOR_SERVICE_UUID_0;
    packet->service_uuid[1] = PROSPECTOR_SERVICE_UUID_1;

    /* Packet type */
    packet->packet_type = PERIODIC_PACKET_TYPE_DYNAMIC;

    /* Layer information */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    packet->active_layer = zmk_keymap_highest_layer_active();

    /* Layer name (max 7 chars + null) */
    const char *layer_name = zmk_keymap_layer_name(packet->active_layer);
    if (layer_name) {
        strncpy(packet->current_layer_name, layer_name, LAYER_NAME_MAX_LEN - 1);
        packet->current_layer_name[LAYER_NAME_MAX_LEN - 1] = '\0';
    } else {
        snprintf(packet->current_layer_name, LAYER_NAME_MAX_LEN, "Layer%d", packet->active_layer);
    }
#else
    packet->active_layer = 0;
    strncpy(packet->current_layer_name, "Layer0", LAYER_NAME_MAX_LEN - 1);
#endif

    /* Modifier flags */
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    if (report) {
        packet->modifier_flags = report->body.modifiers;
    }

    /* Status flags */
    uint8_t flags = 0;

#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_powered()) {
        flags |= STATUS_FLAG_USB_CONNECTED;
    }
    if (zmk_usb_is_hid_ready()) {
        flags |= STATUS_FLAG_USB_HID_READY;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    if (zmk_ble_active_profile_is_connected()) {
        flags |= STATUS_FLAG_BLE_CONNECTED;
    }
    if (!zmk_ble_active_profile_is_open()) {
        flags |= STATUS_FLAG_BLE_BONDED;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_MOUSE) || IS_ENABLED(CONFIG_ZMK_POINTING)
    flags |= STATUS_FLAG_HAS_POINTING;
#endif

    packet->status_flags = flags;

    /* WPM */
    packet->wpm_value = current_wpm;

    /* Battery levels */
    packet->battery_level = zmk_battery_state_of_charge();
    if (packet->battery_level > 100) {
        packet->battery_level = 100;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    packet->peripheral_battery[0] = peripheral_batteries[0];
    packet->peripheral_battery[1] = peripheral_batteries[1];
    packet->peripheral_battery[2] = peripheral_batteries[2];

    /* Peripheral status */
    uint8_t periph_status = 0;
    if (peripheral_batteries[0] > 0) periph_status |= PERIPHERAL_STATUS_0_CONNECTED;
    if (peripheral_batteries[1] > 0) periph_status |= PERIPHERAL_STATUS_1_CONNECTED;
    if (peripheral_batteries[2] > 0) periph_status |= PERIPHERAL_STATUS_2_CONNECTED;
    packet->peripheral_status = periph_status;
#else
    packet->peripheral_battery[0] = 0;
    packet->peripheral_battery[1] = 0;
    packet->peripheral_battery[2] = 0;
    packet->peripheral_status = 0;
#endif

    /* BLE profile */
#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    packet->profile_slot = zmk_ble_active_profile_index();

    /* BLE profile flags (2 bits per profile) */
    uint16_t profile_flags = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t status = BLE_PROFILE_UNUSED;
        if (zmk_ble_profile_is_connected(i)) {
            status = BLE_PROFILE_CONNECTED;
        } else if (!zmk_ble_profile_is_open(i)) {
            status = BLE_PROFILE_BONDED;
        }
        BLE_PROFILE_STATUS_SET(profile_flags, i, status);
    }
    packet->ble_profile_flags = profile_flags;
#else
    packet->profile_slot = 0;
    packet->ble_profile_flags = 0;
#endif

    /* Connection count */
    uint8_t conn_count = 0;
#if IS_ENABLED(CONFIG_ZMK_USB)
    if (zmk_usb_is_hid_ready()) conn_count++;
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE) && (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    if (zmk_ble_active_profile_is_connected()) conn_count++;
#endif
    packet->connection_count = conn_count;

    /* Sequence number */
    packet->sequence_number = sequence_number++;

    /* Pointer data */
    packet->pointer_dx = accumulated_dx;
    packet->pointer_dy = accumulated_dy;
    packet->scroll_v = accumulated_scroll_v;
    packet->scroll_h = accumulated_scroll_h;
    packet->pointer_buttons = current_pointer_buttons;

    /* Reset accumulators after reading */
    accumulated_dx = 0;
    accumulated_dy = 0;
    accumulated_scroll_v = 0;
    accumulated_scroll_h = 0;

    /* Idle time */
    uint32_t now = k_uptime_get_32();
    uint32_t idle_seconds = (now - last_input_time) / 1000;
    packet->idle_seconds_div4 = IDLE_SECONDS_TO_DIV4(idle_seconds);

    /* Indicator flags (Caps/Num/Scroll Lock, Sticky Keys) */
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    zmk_hid_indicators indicators = zmk_hid_indicators_get_current_profile();
    uint8_t ind_flags = 0;
    if (indicators & ZMK_LED_CAPSLOCK_BIT) ind_flags |= INDICATOR_FLAG_CAPS_LOCK;
    if (indicators & ZMK_LED_NUMLOCK_BIT) ind_flags |= INDICATOR_FLAG_NUM_LOCK;
    if (indicators & ZMK_LED_SCROLLLOCK_BIT) ind_flags |= INDICATOR_FLAG_SCROLL_LOCK;
    packet->indicator_flags = ind_flags;
#else
    packet->indicator_flags = 0;
#endif

    /* TODO: Add sticky key detection when available */

    return 0;
}

int periodic_adv_build_static_packet(struct periodic_static_packet *packet) {
    if (!packet) {
        return -EINVAL;
    }

    memset(packet, 0, sizeof(*packet));

    /* Prospector signature (v2.2.0) */
    packet->manufacturer_id[0] = PROSPECTOR_SIGNATURE_0;
    packet->manufacturer_id[1] = PROSPECTOR_SIGNATURE_1;
    packet->service_uuid[0] = PROSPECTOR_SERVICE_UUID_0;
    packet->service_uuid[1] = PROSPECTOR_SERVICE_UUID_1;

    /* Packet type and version */
    packet->packet_type = PERIODIC_PACKET_TYPE_STATIC;
    packet->static_version = PERIODIC_ADV_STATIC_VERSION;

    /* Keyboard ID */
    packet->keyboard_id = generate_keyboard_id();

    /* Layer count */
    packet->layer_count = get_layer_count();

    /* Device role */
    packet->device_role = get_device_role();

    /* Keyboard name (max 23 chars + null) */
    const char *name = CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME;
    strncpy(packet->keyboard_name, name, KEYBOARD_NAME_MAX_LEN - 1);
    packet->keyboard_name[KEYBOARD_NAME_MAX_LEN - 1] = '\0';

    /* Firmware version (placeholder) */
    packet->firmware_version = FW_VERSION_ENCODE(2, 2);  /* v2.2.0 */

    /* Device features */
    packet->device_features = get_device_features();

    /* Layer names (up to 10 layers) */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    for (int i = 0; i < STATIC_LAYER_COUNT; i++) {
        const char *layer_name = zmk_keymap_layer_name(i);
        if (layer_name) {
            strncpy(packet->layer_names[i], layer_name, LAYER_NAME_MAX_LEN - 1);
            packet->layer_names[i][LAYER_NAME_MAX_LEN - 1] = '\0';
        }
    }
#endif

    /* Statistics */
    packet->total_keypress = total_keypress_count;
    packet->boot_count = boot_count;

    /* Zephyr version */
    packet->zephyr_version = FW_VERSION_ENCODE(KERNEL_VERSION_MAJOR, KERNEL_VERSION_MINOR);

    /* Peripheral RSSI (update before building packet) */
    update_peripheral_rssi();
    packet->peripheral_rssi[0] = cached_rssi[0];
    packet->peripheral_rssi[1] = cached_rssi[1];
    packet->peripheral_rssi[2] = cached_rssi[2];

    return 0;
}

/*============================================================================
 * Periodic Advertising Management
 *============================================================================*/

static void update_periodic_advertising_data(bool send_static) {
    if (!per_adv_set || !per_adv_started) {
        return;
    }

    int err;

    if (send_static) {
        /* Build and send static packet */
        periodic_adv_build_static_packet(&static_packet);

        struct bt_data per_ad[] = {
            BT_DATA(BT_DATA_MANUFACTURER_DATA,
                    (uint8_t *)&static_packet,
                    sizeof(static_packet)),
        };

        err = bt_le_per_adv_set_data(per_adv_set, per_ad, ARRAY_SIZE(per_ad));
        if (err) {
            LOG_WRN("Failed to set static periodic data: %d", err);
        } else {
            LOG_DBG("Static packet sent (%d bytes)", STATIC_PACKET_SIZE);
        }

        static_update_requested = false;
    } else {
        /* Build and send dynamic packet */
        periodic_adv_build_dynamic_packet(&dynamic_packet);

        struct bt_data per_ad[] = {
            BT_DATA(BT_DATA_MANUFACTURER_DATA,
                    (uint8_t *)&dynamic_packet,
                    sizeof(dynamic_packet)),
        };

        err = bt_le_per_adv_set_data(per_adv_set, per_ad, ARRAY_SIZE(per_ad));
        if (err) {
            LOG_WRN("Failed to set dynamic periodic data: %d", err);
        } else {
            LOG_DBG("Dynamic packet sent (%d bytes, seq=%d, layer=%d)",
                    DYNAMIC_PACKET_SIZE, dynamic_packet.sequence_number, dynamic_packet.active_layer);
        }
    }
}

static void dynamic_work_handler(struct k_work *work) {
    if (!per_adv_started) {
        return;
    }

    /* Send dynamic packet */
    update_periodic_advertising_data(false);

    /* Periodic status log every 5 seconds (for debugging) */
    static uint32_t last_status_log = 0;
    uint32_t now = k_uptime_get_32();
    if (now - last_status_log > 5000) {
        last_status_log = now;
        struct bt_le_ext_adv_info adv_info;
        if (per_adv_set && bt_le_ext_adv_get_info(per_adv_set, &adv_info) == 0) {
            LOG_INF("📡 PERIODIC STATUS: set=%p, started=%d, SID=%d, seq=%d",
                    (void*)per_adv_set, per_adv_started, adv_info.id, sequence_number);
        } else {
            LOG_WRN("📡 PERIODIC STATUS: set=%p, started=%d, info FAILED",
                    (void*)per_adv_set, per_adv_started);
        }
    }

    /* Schedule next dynamic update */
    k_work_schedule(&dynamic_work, K_MSEC(DYNAMIC_INTERVAL_MS));
}

static void static_work_handler(struct k_work *work) {
    if (!per_adv_started) {
        return;
    }

    /* Send static packet */
    update_periodic_advertising_data(true);

    /* Schedule next static update */
    k_work_schedule(&static_work, K_MSEC(STATIC_INTERVAL_MS));
}

/*============================================================================
 * Public API
 *============================================================================*/

int periodic_adv_protocol_init(void) {
    k_work_init_delayable(&dynamic_work, dynamic_work_handler);
    k_work_init_delayable(&static_work, static_work_handler);

    last_input_time = k_uptime_get_32();
    boot_count++;  /* Increment boot count */

    LOG_INF("Periodic ADV Protocol initialized (dynamic: %dms, static: %dms)",
            DYNAMIC_INTERVAL_MS, STATIC_INTERVAL_MS);

    return 0;
}

int periodic_adv_protocol_start(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* Skip on peripheral devices */
    LOG_INF("📡 Periodic ADV skipped - this is a split peripheral");
    return 0;
#endif

    LOG_INF("📡 Starting Periodic ADV setup...");
    int err;

    /* Create Extended Advertising Set for Periodic ADV */
    if (!per_adv_set) {
        /* Use BT_LE_ADV_OPT_NO_2M to force 1M PHY for better compatibility */
        static const struct bt_le_adv_param ext_adv_param = BT_LE_ADV_PARAM_INIT(
            BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_NO_2M,
            BT_GAP_ADV_FAST_INT_MIN_2,
            BT_GAP_ADV_FAST_INT_MAX_2,
            NULL);

        err = bt_le_ext_adv_create(&ext_adv_param, NULL, &per_adv_set);
        if (err) {
            LOG_ERR("Failed to create periodic ext adv set: %d", err);
            return err;
        }
    }

    /* Set Periodic Advertising parameters */
    struct bt_le_per_adv_param per_param = {
        .interval_min = MS_TO_INTERVAL(DYNAMIC_INTERVAL_MS),
        .interval_max = MS_TO_INTERVAL(DYNAMIC_INTERVAL_MS),
        .options = 0,
    };

    err = bt_le_per_adv_set_param(per_adv_set, &per_param);
    if (err) {
        LOG_ERR("Failed to set periodic adv params: %d", err);
        return err;
    }

    /* Set initial data (static packet for sync establishment) */
    periodic_adv_build_static_packet(&static_packet);
    struct bt_data per_ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA,
                (uint8_t *)&static_packet,
                sizeof(static_packet)),
    };

    err = bt_le_per_adv_set_data(per_adv_set, per_ad, ARRAY_SIZE(per_ad));
    if (err) {
        LOG_ERR("Failed to set initial periodic data: %d", err);
        return err;
    }

    /* Start Extended Advertising (carrier for SyncInfo) */
    err = bt_le_ext_adv_start(per_adv_set, BT_LE_EXT_ADV_START_DEFAULT);
    if (err && err != -EALREADY) {
        LOG_ERR("Failed to start periodic ext adv: %d", err);
        return err;
    }

    /* Start Periodic Advertising */
    err = bt_le_per_adv_start(per_adv_set);
    if (err && err != -EALREADY) {
        LOG_ERR("Failed to start periodic adv: %d", err);
        return err;
    }

    per_adv_started = true;

    /* Start work handlers */
    k_work_schedule(&dynamic_work, K_MSEC(DYNAMIC_INTERVAL_MS));
    k_work_schedule(&static_work, K_MSEC(STATIC_INTERVAL_MS));

    /* Log the SID of this advertising set */
    struct bt_le_ext_adv_info adv_info;
    if (bt_le_ext_adv_get_info(per_adv_set, &adv_info) == 0) {
        LOG_INF("📡 Periodic Advertising started on SID=%d (dynamic: %dms, static: %dms)",
                adv_info.id, DYNAMIC_INTERVAL_MS, STATIC_INTERVAL_MS);
    } else {
        LOG_INF("Periodic Advertising started (dynamic: %dms, static: %dms)",
                DYNAMIC_INTERVAL_MS, STATIC_INTERVAL_MS);
    }

    return 0;
}

int periodic_adv_protocol_stop(void) {
    if (!per_adv_started) {
        return 0;
    }

    /* Cancel work handlers */
    k_work_cancel_delayable(&dynamic_work);
    k_work_cancel_delayable(&static_work);

    /* Stop Periodic Advertising */
    if (per_adv_set) {
        bt_le_per_adv_stop(per_adv_set);
        bt_le_ext_adv_stop(per_adv_set);
        bt_le_ext_adv_delete(per_adv_set);
        per_adv_set = NULL;
    }

    per_adv_started = false;
    LOG_INF("Periodic Advertising stopped");

    return 0;
}

void periodic_adv_request_static_update(void) {
    static_update_requested = true;

    /* Schedule immediate static update */
    if (per_adv_started) {
        k_work_cancel_delayable(&static_work);
        k_work_schedule(&static_work, K_NO_WAIT);
    }
}

void periodic_adv_update_pointer(int16_t dx, int16_t dy) {
    accumulated_dx += dx;
    accumulated_dy += dy;
    last_input_time = k_uptime_get_32();
}

void periodic_adv_update_scroll(int8_t v, int8_t h) {
    /* Clamp accumulated values to int8 range */
    int16_t new_v = accumulated_scroll_v + v;
    int16_t new_h = accumulated_scroll_h + h;

    accumulated_scroll_v = (new_v > 127) ? 127 : (new_v < -128) ? -128 : (int8_t)new_v;
    accumulated_scroll_h = (new_h > 127) ? 127 : (new_h < -128) ? -128 : (int8_t)new_h;
    last_input_time = k_uptime_get_32();
}

void periodic_adv_update_pointer_buttons(uint8_t buttons) {
    current_pointer_buttons = buttons;
    last_input_time = k_uptime_get_32();
}

/* Hook into key press events for activity tracking */
#include <zmk/events/position_state_changed.h>
#include <zmk/event_manager.h>

static int periodic_adv_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev && ev->state) {
        last_input_time = k_uptime_get_32();
        total_keypress_count++;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(periodic_adv_position, periodic_adv_position_listener);
ZMK_SUBSCRIPTION(periodic_adv_position, zmk_position_state_changed);

#endif /* CONFIG_ZMK_STATUS_ADV_PERIODIC */
