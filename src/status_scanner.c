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

static int find_keyboard_by_id_and_role(uint32_t keyboard_id, uint8_t device_role) {
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && 
            memcmp(keyboards[i].data.keyboard_id, &keyboard_id, 4) == 0 &&
            keyboards[i].data.device_role == device_role) {
            return i;
        }
    }
    return -1;
}

static int find_keyboard_by_id(uint32_t keyboard_id) {
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            uint32_t stored_id = get_keyboard_id_from_data(&keyboards[i].data);
            if (stored_id == keyboard_id) {
                return i;
            }
        }
    }
    return -1;
}

static int find_keyboard_by_id_and_role(uint32_t keyboard_id, uint8_t device_role) {
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            uint32_t stored_id = get_keyboard_id_from_data(&keyboards[i].data);
            if (stored_id == keyboard_id && keyboards[i].data.device_role == device_role) {
                printk("*** SCANNER: Found existing slot %d for %s ID=%08X ***\n", 
                       i, (device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" : 
                          (device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE",
                       keyboard_id);
                return i;
            }
        }
    }
    printk("*** SCANNER: No existing slot found for %s ID=%08X ***\n", 
           (device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" : 
           (device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE",
           keyboard_id);
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
    
    // Debug: Print what we received
    const char *role_str = "UNKNOWN";
    if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";
    
    printk("*** SCANNER DEBUG: Received %s, ID=%08X, Battery=%d%%, Layer=%d ***\n",
           role_str, keyboard_id, adv_data->battery_level, adv_data->active_layer);
    
    // Find existing keyboard by ID AND role for split keyboards
    int index = find_keyboard_by_id_and_role(keyboard_id, adv_data->device_role);
    bool is_new = false;
    
    if (index < 0) {
        // Find empty slot for new keyboard
        index = find_empty_slot();
        if (index < 0) {
            LOG_WRN("No empty slots for new keyboard");
            return;
        }
        is_new = true;
        printk("*** SCANNER: Creating NEW slot %d for %s ID=%08X ***\n", 
               index, role_str, keyboard_id);
    }
    
    // Update keyboard status
    keyboards[index].active = true;
    keyboards[index].last_seen = now;
    keyboards[index].rssi = rssi;
    memcpy(&keyboards[index].data, adv_data, sizeof(struct zmk_status_adv_data));
    
    // Debug: Print current active slots
    if (is_new) {
        printk("*** SCANNER: Current active slots: ***\n");
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            if (keyboards[i].active) {
                uint32_t id = get_keyboard_id_from_data(&keyboards[i].data);
                const char *role = (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" : 
                                  (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE";
                printk("*** SLOT %d: %s ID=%08X Battery=%d%% ***\n", 
                       i, role, id, keyboards[i].data.battery_level);
            }
        }
    }
    
    // Notify event
    if (is_new) {
        const char *role_str = "UNKNOWN";
        if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";
        
        printk("*** PROSPECTOR SCANNER: New %s device found: %s (slot %d) ***\n", role_str, adv_data->layer_name, index);
        LOG_INF("New %s device found: %s (slot %d)", role_str, adv_data->layer_name, index);
        notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND, index);
    } else {
        const char *role_str = "UNKNOWN";
        if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL"; 
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";
        
        printk("*** PROSPECTOR SCANNER: %s device updated: %s, battery: %d%% ***\n", 
               role_str, adv_data->layer_name, adv_data->battery_level);
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
        
        // Check for compact 6-byte Prospector manufacturer data
        if (ad_type == BT_DATA_MANUFACTURER_DATA && len >= 6) {
            printk("*** PROSPECTOR SCANNER: Found manufacturer data! len=%d ***\n", len);
            
            const uint8_t *data = buf->data;
            
            // Check if this is Prospector data: FF FF AB CD
            if (data[0] == 0xFF && data[1] == 0xFF && 
                data[2] == 0xAB && data[3] == 0xCD) {
                
                printk("*** PROSPECTOR SCANNER: Valid Prospector data found! ***\n");
                printk("*** PROSPECTOR SCANNER: Data: %02X %02X %02X %02X %02X %02X ***\n",
                       data[0], data[1], data[2], data[3], data[4], data[5]);
                
                // Create zmk_status_adv_data from compact payload
                struct zmk_status_adv_data adv_data = {0};
                adv_data.manufacturer_id[0] = data[0];
                adv_data.manufacturer_id[1] = data[1];
                adv_data.service_uuid[0] = data[2];
                adv_data.service_uuid[1] = data[3];
                adv_data.version = ZMK_STATUS_ADV_VERSION;
                adv_data.battery_level = data[4];
                
                // Parse combined layer + status byte
                uint8_t combined = data[5];
                adv_data.active_layer = combined & 0x0F;  // Lower 4 bits
                adv_data.status_flags = 0;
                if (combined & 0x10) adv_data.status_flags |= ZMK_STATUS_FLAG_USB_CONNECTED;
                if (combined & 0x40) adv_data.device_role = ZMK_DEVICE_ROLE_CENTRAL;
                else if (combined & 0x80) adv_data.device_role = ZMK_DEVICE_ROLE_PERIPHERAL;
                else adv_data.device_role = ZMK_DEVICE_ROLE_STANDALONE;
                
                // Set defaults for missing data
                adv_data.profile_slot = 0;
                adv_data.connection_count = 1;
                snprintf(adv_data.layer_name, sizeof(adv_data.layer_name), "L%d", adv_data.active_layer);
                
                // Generate keyboard ID - use base name for grouping split keyboards
                const char *name = "LalaPad"; // TODO: Get from scan response
                uint32_t hash = 0;
                for (int i = 0; name[i] != '\0'; i++) {
                    hash = hash * 31 + name[i];
                }
                
                // For split keyboards, use the same base ID for grouping
                // But set device_index based on role for proper tracking
                adv_data.keyboard_id[0] = (hash >> 24) & 0xFF;
                adv_data.keyboard_id[1] = (hash >> 16) & 0xFF;
                adv_data.keyboard_id[2] = (hash >> 8) & 0xFF;
                adv_data.keyboard_id[3] = hash & 0xFF;
                
                // Set device_index based on role
                if (adv_data.device_role == ZMK_DEVICE_ROLE_CENTRAL) {
                    adv_data.device_index = 0;
                } else if (adv_data.device_role == ZMK_DEVICE_ROLE_PERIPHERAL) {
                    adv_data.device_index = 1;
                } else {
                    adv_data.device_index = 0;
                }
                
                process_advertisement(&adv_data, rssi);
            } else {
                printk("*** PROSPECTOR SCANNER: Not Prospector data - got %02X %02X %02X %02X ***\n",
                       data[0], data[1], data[2], data[3]);
            }
        }
        
        net_buf_simple_pull(buf, len);
    }
}

static void timeout_work_handler(struct k_work *work) {
    uint32_t now = k_uptime_get_32();
    
    printk("*** PROSPECTOR SCANNER: Timeout check at time %u ***\n", now);
    
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            uint32_t age = now - keyboards[i].last_seen;
            printk("*** SCANNER: Slot %d age: %ums (timeout at %ums) ***\n", 
                   i, age, KEYBOARD_TIMEOUT_MS);
            
            if (age > KEYBOARD_TIMEOUT_MS) {
                printk("*** SCANNER: TIMEOUT! Removing keyboard %s from slot %d ***\n", 
                       keyboards[i].data.layer_name, i);
                LOG_INF("Keyboard timeout: %s (slot %d)", keyboards[i].data.layer_name, i);
                keyboards[i].active = false;
                notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_LOST, i);
            }
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
    uint32_t now = k_uptime_get_32();
    
    printk("*** PROSPECTOR SCANNER: Active keyboard check at time %u ***\n", now);
    
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            uint32_t age = now - keyboards[i].last_seen;
            printk("*** SCANNER: Slot %d: ACTIVE, last_seen=%u, age=%ums, name=%s ***\n", 
                   i, keyboards[i].last_seen, age, keyboards[i].data.layer_name);
            count++;
        } else {
            printk("*** SCANNER: Slot %d: INACTIVE ***\n", i);
        }
    }
    
    printk("*** PROSPECTOR SCANNER: Total active count: %d ***\n", count);
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