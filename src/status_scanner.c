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

// Helper function to get keyboard ID
static uint32_t get_keyboard_id_from_data(const struct zmk_status_adv_data *data) {
    return (data->keyboard_id[0] << 24) | 
           (data->keyboard_id[1] << 16) | 
           (data->keyboard_id[2] << 8) | 
           data->keyboard_id[3];
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

// Forward declaration
static const char* get_device_name(const bt_addr_le_t *addr);

static void process_advertisement_with_name(const struct zmk_status_adv_data *adv_data, int8_t rssi, const bt_addr_le_t *addr) {
    uint32_t now = k_uptime_get_32();
    uint32_t keyboard_id = get_keyboard_id_from_data(adv_data);
    
    // Get device name
    const char *device_name = get_device_name(addr);
    
    const char *role_str = "UNKNOWN";
    if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";
    
    printk("*** SCANNER DEBUG: Received %s (%s), ID=%08X, Battery=%d%%, Layer=%d ***\n",
           role_str, device_name, keyboard_id, adv_data->battery_level, adv_data->active_layer);
    
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
        printk("*** SCANNER: Creating NEW slot %d for %s (%s) ID=%08X ***\n", 
               index, role_str, device_name, keyboard_id);
    }
    
    // Update keyboard status
    keyboards[index].active = true;
    keyboards[index].last_seen = now;
    keyboards[index].rssi = rssi;
    memcpy(&keyboards[index].data, adv_data, sizeof(struct zmk_status_adv_data));
    strncpy(keyboards[index].ble_name, device_name, sizeof(keyboards[index].ble_name) - 1);
    keyboards[index].ble_name[sizeof(keyboards[index].ble_name) - 1] = '\0';
    
    // Debug: Print current active slots
    if (is_new) {
        printk("*** SCANNER: Current active slots: ***\n");
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            if (keyboards[i].active) {
                uint32_t id = get_keyboard_id_from_data(&keyboards[i].data);
                const char *role = (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" : 
                                  (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE";
                printk("*** SLOT %d: %s (%s) ID=%08X Battery=%d%% ***\n", 
                       i, role, keyboards[i].ble_name, id, keyboards[i].data.battery_level);
            }
        }
    }
    
    // Notify event
    if (is_new) {
        const char *role_str = "UNKNOWN";
        if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";
        
        printk("*** PROSPECTOR SCANNER: New %s device found: %s (slot %d) ***\n", role_str, device_name, index);
        LOG_INF("New %s device found: %s (slot %d)", role_str, device_name, index);
        notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND, index);
    } else {
        const char *role_str = "UNKNOWN";
        if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL"; 
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";
        
        printk("*** PROSPECTOR SCANNER: %s device updated: %s, battery: %d%% ***\n", 
               role_str, device_name, adv_data->battery_level);
        notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_UPDATED, index);
    }
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

// Temporary storage for device name and data correlation
static struct {
    bt_addr_le_t addr;
    char name[32];
    uint32_t timestamp;
} temp_device_names[5];

static void store_device_name(const bt_addr_le_t *addr, const char *name) {
    uint32_t now = k_uptime_get_32();
    // Find or create slot for this address
    for (int i = 0; i < 5; i++) {
        if (bt_addr_le_cmp(&temp_device_names[i].addr, addr) == 0 ||
            (now - temp_device_names[i].timestamp) > 5000) { // Expire after 5 seconds
            bt_addr_le_copy(&temp_device_names[i].addr, addr);
            strncpy(temp_device_names[i].name, name, sizeof(temp_device_names[i].name) - 1);
            temp_device_names[i].name[sizeof(temp_device_names[i].name) - 1] = '\0';
            temp_device_names[i].timestamp = now;
            break;
        }
    }
}

static const char* get_device_name(const bt_addr_le_t *addr) {
    uint32_t now = k_uptime_get_32();
    for (int i = 0; i < 5; i++) {
        if (bt_addr_le_cmp(&temp_device_names[i].addr, addr) == 0 &&
            (now - temp_device_names[i].timestamp) <= 5000) {
            return temp_device_names[i].name;
        }
    }
    return "Unknown";
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
    
    const struct zmk_status_adv_data *prospector_data = NULL;
    
    // Parse advertisement data to extract both name and Prospector data
    struct net_buf_simple buf_copy = *buf; // Make a copy for parsing
    while (buf_copy.len > 1) {
        uint8_t len = net_buf_simple_pull_u8(&buf_copy);
        if (len == 0 || len > buf_copy.len) {
            break;
        }
        
        uint8_t ad_type = net_buf_simple_pull_u8(&buf_copy);
        len--; // Subtract type byte
        
        // Extract device name
        if ((ad_type == BT_DATA_NAME_COMPLETE || ad_type == BT_DATA_NAME_SHORTENED) && len > 0) {
            char device_name[32];
            memcpy(device_name, buf_copy.data, MIN(len, sizeof(device_name) - 1));
            device_name[MIN(len, sizeof(device_name) - 1)] = '\0';
            store_device_name(addr, device_name);
            printk("*** PROSPECTOR SCANNER: Found device name: %s ***\n", device_name);
        }
        
        // Check for Prospector manufacturer data
        if (ad_type == BT_DATA_MANUFACTURER_DATA && len >= sizeof(struct zmk_status_adv_data)) {
            const struct zmk_status_adv_data *data = (const struct zmk_status_adv_data *)buf_copy.data;
            
            // Check if this is Prospector data: FF FF AB CD
            if (data->manufacturer_id[0] == 0xFF && data->manufacturer_id[1] == 0xFF && 
                data->service_uuid[0] == 0xAB && data->service_uuid[1] == 0xCD) {
                prospector_data = data;
                printk("*** PROSPECTOR SCANNER: Valid Prospector data found! Version=%d ***\n", data->version);
            }
        }
        
        net_buf_simple_pull(&buf_copy, len);
    }
    
    // Process Prospector data if found
    if (prospector_data) {
        printk("*** PROSPECTOR SCANNER: Central=%d%%, Peripheral=[%d,%d,%d], Layer=%d ***\n",
               prospector_data->battery_level, prospector_data->peripheral_battery[0], 
               prospector_data->peripheral_battery[1], prospector_data->peripheral_battery[2], 
               prospector_data->active_layer);
        
        // Process advertisement with device name
        process_advertisement_with_name(prospector_data, rssi, addr);
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