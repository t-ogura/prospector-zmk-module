/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/buf.h>
#include <zephyr/init.h>
#include <string.h>

#include <zmk/status_scanner.h>
#include <zmk/status_advertisement.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER)

static struct zmk_keyboard_status keyboards[ZMK_STATUS_SCANNER_MAX_KEYBOARDS];
static zmk_status_scanner_callback_t event_callback = NULL;
static bool scanning = false;
static struct k_work_delayable timeout_work;

// Timeout for considering a keyboard as lost (in milliseconds)
#define KEYBOARD_TIMEOUT_MS 10000

static void notify_event(enum zmk_status_scanner_event event, int keyboard_index) {
    if (event_callback) {
        struct zmk_status_scanner_event_data event_data = {
            .event = event,
            .keyboard_index = keyboard_index,
            .status = &keyboards[keyboard_index]
        };
        event_callback(&event_data);
    }
}

static int find_keyboard_by_id(uint32_t keyboard_id) {
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && 
            memcmp(keyboards[i].data.keyboard_id, &keyboard_id, 4) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_empty_slot(void) {
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (!keyboards[i].active) {
            return i;
        }
    }
    return -1;
}

static uint32_t get_keyboard_id_from_data(const struct zmk_status_adv_data *data) {
    return (data->keyboard_id[0] << 24) | 
           (data->keyboard_id[1] << 16) | 
           (data->keyboard_id[2] << 8) | 
           data->keyboard_id[3];
}

static void process_advertisement(const struct zmk_status_adv_data *adv_data, int8_t rssi) {
    uint32_t keyboard_id = get_keyboard_id_from_data(adv_data);
    uint32_t now = k_uptime_get_32();
    
    // Find existing keyboard
    int index = find_keyboard_by_id(keyboard_id);
    bool is_new = false;
    
    if (index < 0) {
        // Find empty slot for new keyboard
        index = find_empty_slot();
        if (index < 0) {
            LOG_WRN("No empty slots for new keyboard");
            return;
        }
        is_new = true;
    }
    
    // Update keyboard status
    keyboards[index].active = true;
    keyboards[index].last_seen = now;
    keyboards[index].rssi = rssi;
    memcpy(&keyboards[index].data, adv_data, sizeof(struct zmk_status_adv_data));
    
    // Notify event
    if (is_new) {
        printk("*** PROSPECTOR SCANNER: New keyboard found: %s (slot %d) ***\n", adv_data->layer_name, index);
        LOG_INF("New keyboard found: %s (slot %d)", adv_data->layer_name, index);
        notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND, index);
    } else {
        printk("*** PROSPECTOR SCANNER: Keyboard updated: %s, battery: %d%% ***\n", 
               adv_data->layer_name, adv_data->battery_level);
        notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_UPDATED, index);
    }
}

static void scan_callback(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *buf) {
    static int scan_count = 0;
    scan_count++;
    
    // Log every 10th scan to avoid spam
    if (scan_count % 10 == 1) {
        printk("*** PROSPECTOR SCANNER: Received BLE adv %d, RSSI: %d, len: %d ***\n", 
               scan_count, rssi, buf->len);
    }
    
    if (!scanning) {
        return;
    }
    
    // Parse advertisement data
    while (buf->len > 1) {
        uint8_t len = net_buf_simple_pull_u8(buf);
        if (len == 0 || len > buf->len) {
            break;
        }
        
        uint8_t ad_type = net_buf_simple_pull_u8(buf);
        len--; // Subtract type byte
        
        if (ad_type == BT_DATA_MANUFACTURER_DATA && len >= sizeof(struct zmk_status_adv_data)) {
            printk("*** PROSPECTOR SCANNER: Found manufacturer data! len=%d, expected=%d ***\n", 
                   len, sizeof(struct zmk_status_adv_data));
            
            const struct zmk_status_adv_data *adv_data = 
                (const struct zmk_status_adv_data *)buf->data;
            
            // Check if this is a valid status advertisement
            printk("*** PROSPECTOR SCANNER: Checking UUID - got %02X%02X, expect %04X ***\n",
                   adv_data->service_uuid[0], adv_data->service_uuid[1], ZMK_STATUS_ADV_SERVICE_UUID);
            
            if (adv_data->manufacturer_id[0] == 0xFF && 
                adv_data->manufacturer_id[1] == 0xFF &&
                adv_data->service_uuid[0] == ((ZMK_STATUS_ADV_SERVICE_UUID >> 8) & 0xFF) &&
                adv_data->service_uuid[1] == (ZMK_STATUS_ADV_SERVICE_UUID & 0xFF) &&
                adv_data->version == ZMK_STATUS_ADV_VERSION) {
                
                printk("*** PROSPECTOR SCANNER: Valid Prospector advertisement found! ***\n");
                process_advertisement(adv_data, rssi);
            } else {
                printk("*** PROSPECTOR SCANNER: Invalid advertisement - wrong UUID or version ***\n");
            }
        }
        
        net_buf_simple_pull(buf, len);
    }
}

static void timeout_work_handler(struct k_work *work) {
    uint32_t now = k_uptime_get_32();
    
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && 
            (now - keyboards[i].last_seen) > KEYBOARD_TIMEOUT_MS) {
            
            LOG_INF("Keyboard timeout: %s (slot %d)", keyboards[i].data.layer_name, i);
            keyboards[i].active = false;
            notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_LOST, i);
        }
    }
    
    // Reschedule timeout check
    if (scanning) {
        k_work_schedule(&timeout_work, K_MSEC(KEYBOARD_TIMEOUT_MS / 2));
    }
}

int zmk_status_scanner_init(void) {
    memset(keyboards, 0, sizeof(keyboards));
    k_work_init_delayable(&timeout_work, timeout_work_handler);
    
    LOG_INF("Status scanner initialized");
    return 0;
}

int zmk_status_scanner_start(void) {
    if (scanning) {
        return 0;
    }
    
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    
    int err = bt_le_scan_start(&scan_param, scan_callback);
    if (err) {
        LOG_ERR("Failed to start scanning: %d", err);
        return err;
    }
    
    scanning = true;
    k_work_schedule(&timeout_work, K_MSEC(KEYBOARD_TIMEOUT_MS / 2));
    
    LOG_INF("Status scanner started");
    return 0;
}

int zmk_status_scanner_stop(void) {
    if (!scanning) {
        return 0;
    }
    
    scanning = false;
    k_work_cancel_delayable(&timeout_work);
    
    int err = bt_le_scan_stop();
    if (err) {
        LOG_ERR("Failed to stop scanning: %d", err);
        return err;
    }
    
    LOG_INF("Status scanner stopped");
    return 0;
}

int zmk_status_scanner_register_callback(zmk_status_scanner_callback_t callback) {
    event_callback = callback;
    return 0;
}

struct zmk_keyboard_status *zmk_status_scanner_get_keyboard(int index) {
    if (index < 0 || index >= ZMK_STATUS_SCANNER_MAX_KEYBOARDS) {
        return NULL;
    }
    
    return keyboards[index].active ? &keyboards[index] : NULL;
}

int zmk_status_scanner_get_active_count(void) {
    int count = 0;
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            count++;
        }
    }
    return count;
}

int zmk_status_scanner_get_primary_keyboard(void) {
    int primary = -1;
    uint32_t latest_seen = 0;
    
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && keyboards[i].last_seen > latest_seen) {
            latest_seen = keyboards[i].last_seen;
            primary = i;
        }
    }
    
    return primary;
}

// Initialize on system startup - use later priority to ensure BT is ready
SYS_INIT(zmk_status_scanner_init, APPLICATION, 99);

#endif // CONFIG_PROSPECTOR_MODE_SCANNER