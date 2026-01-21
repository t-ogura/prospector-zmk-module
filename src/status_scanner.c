/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/init.h>
#include <string.h>

#include <zmk/status_scanner.h>
#include <zmk/status_advertisement.h>

// Periodic Sync support (v2.2.0)
#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
#include <zephyr/bluetooth/gap.h>

// Extended Scanning callback for SID discovery
// bt_le_scan_cb_register() provides bt_le_scan_recv_info with SID field
static void extended_scan_recv(const struct bt_le_scan_recv_info *info,
                                struct net_buf_simple *buf);

static struct bt_le_scan_cb extended_scan_callbacks = {
    .recv = extended_scan_recv,
};

static bool extended_scan_cb_registered = false;

// Cache for discovered SIDs from periodic advertisers
// Maps BLE address -> SID (since periodic and legacy ADV use same identity address)
#define PERIODIC_ADDR_CACHE_SIZE 5
static struct {
    bt_addr_le_t addr;
    uint8_t sid;
    uint32_t timestamp;
    bool valid;
} periodic_addr_cache[PERIODIC_ADDR_CACHE_SIZE];

// Note: Can't use LOG_INF here - defined before LOG_MODULE_DECLARE
// Logging will be done by caller in extended_scan_recv
static void cache_periodic_sid(const bt_addr_le_t *addr, uint8_t sid) {
    uint32_t now = k_uptime_get_32();
    int oldest_idx = 0;
    uint32_t oldest_time = UINT32_MAX;

    // Find existing entry or oldest slot
    for (int i = 0; i < PERIODIC_ADDR_CACHE_SIZE; i++) {
        if (periodic_addr_cache[i].valid &&
            bt_addr_le_cmp(&periodic_addr_cache[i].addr, addr) == 0) {
            // Update existing entry
            periodic_addr_cache[i].sid = sid;
            periodic_addr_cache[i].timestamp = now;
            return;
        }
        if (!periodic_addr_cache[i].valid ||
            periodic_addr_cache[i].timestamp < oldest_time) {
            oldest_time = periodic_addr_cache[i].timestamp;
            oldest_idx = i;
        }
    }

    // Add new entry
    bt_addr_le_copy(&periodic_addr_cache[oldest_idx].addr, addr);
    periodic_addr_cache[oldest_idx].sid = sid;
    periodic_addr_cache[oldest_idx].timestamp = now;
    periodic_addr_cache[oldest_idx].valid = true;
}

static int get_cached_sid(const bt_addr_le_t *addr) {
    for (int i = 0; i < PERIODIC_ADDR_CACHE_SIZE; i++) {
        if (periodic_addr_cache[i].valid &&
            bt_addr_le_cmp(&periodic_addr_cache[i].addr, addr) == 0) {
            return periodic_addr_cache[i].sid;
        }
    }
    return -1;  // Not found
}
#endif

// Scanner stub functions for thread-safe display updates
// Include path assumes build from zmk-config-prospector
#include "../boards/shields/prospector_scanner/src/scanner_stub.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// External function to trigger high-priority display update (defined in scanner_display.c)
// Uses weak reference so it compiles even if scanner_display.c is not in the build
__attribute__((weak)) void scanner_trigger_high_priority_update(void) {
    // Default implementation: do nothing
    // Will be overridden by scanner_display.c if included in build
}

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER)

static struct zmk_keyboard_status keyboards[ZMK_STATUS_SCANNER_MAX_KEYBOARDS];
static zmk_status_scanner_callback_t event_callback = NULL;
static bool scanning = false;
static struct k_work_delayable timeout_work;

// Mutex for thread-safe access to keyboards array
// Protects against concurrent access from BLE scan callback, timeout handler, and API calls
static struct k_mutex scanner_mutex;
static bool scanner_mutex_initialized = false;

// Periodic Sync state tracking (v2.2.0)
#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
static struct bt_le_per_adv_sync *per_sync = NULL;
static sync_state_t current_sync_state = SYNC_STATE_NONE;
static int selected_keyboard_index = -1;
static uint8_t sync_retry_count = 0;
static struct k_work_delayable sync_retry_work;

#define SYNC_TIMEOUT_MS      10000   // 10 seconds sync timeout
#define SYNC_RETRY_DELAY_MS  3000    // 3 seconds retry delay
#define SYNC_MAX_RETRIES     1       // Retry once before fallback

// Forward declarations for Periodic sync functions
static int start_periodic_sync(int keyboard_index);
static void stop_periodic_sync(void);
static void sync_retry_work_handler(struct k_work *work);
#endif // CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC

static void scanner_mutex_init(void) {
    if (!scanner_mutex_initialized) {
        k_mutex_init(&scanner_mutex);
        scanner_mutex_initialized = true;
        LOG_DBG("🔒 Scanner mutex initialized");
    }
}

static int scanner_lock(k_timeout_t timeout) {
    if (!scanner_mutex_initialized) {
        return -EINVAL;
    }
    return k_mutex_lock(&scanner_mutex, timeout);
}

static void scanner_unlock(void) {
    if (scanner_mutex_initialized) {
        k_mutex_unlock(&scanner_mutex);
    }
}

// Timeout for considering a keyboard as lost (in milliseconds)
#ifdef CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS
#define KEYBOARD_TIMEOUT_MS CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS
#else
#define KEYBOARD_TIMEOUT_MS 300000  // 5 minutes default
#endif

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
                LOG_DBG("Found existing slot %d for %s ID=%08X",
                       i, (device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" :
                          (device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE",
                       keyboard_id);
                return i;
            }
        }
    }
    LOG_DBG("No existing slot found for %s ID=%08X",
           (device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" :
           (device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE",
           keyboard_id);
    return -1;
}

// Find keyboard by BLE address - more reliable than name-based ID for same-name keyboards
static int find_keyboard_by_ble_addr(const bt_addr_le_t *addr) {
    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            // Compare BLE address (6 bytes)
            if (memcmp(keyboards[i].ble_addr, addr->a.val, 6) == 0) {
                LOG_DBG("Found existing slot %d by BLE addr %02X:%02X:%02X:%02X:%02X:%02X",
                       i, addr->a.val[5], addr->a.val[4], addr->a.val[3],
                       addr->a.val[2], addr->a.val[1], addr->a.val[0]);
                return i;
            }
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

// Forward declaration
static const char* get_device_name(const bt_addr_le_t *addr);

static void process_advertisement_with_name(const struct zmk_status_adv_data *adv_data, int8_t rssi, const bt_addr_le_t *addr) {
    // Acquire mutex for keyboard array access (non-blocking to avoid BLE stack issues)
    if (scanner_lock(K_MSEC(5)) != 0) {
        LOG_DBG("Advertisement processing skipped - mutex busy");
        return;
    }

    uint32_t now = k_uptime_get_32();
    uint32_t keyboard_id = get_keyboard_id_from_data(adv_data);

    // Get device name
    const char *device_name = get_device_name(addr);

    const char *role_str = "UNKNOWN";
    if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";

    LOG_DBG("Received %s (%s), ID=%08X, BLE=%02X:%02X:%02X:%02X:%02X:%02X, Battery=%d%%, Layer=%d",
           role_str, device_name, keyboard_id,
           addr->a.val[5], addr->a.val[4], addr->a.val[3],
           addr->a.val[2], addr->a.val[1], addr->a.val[0],
           adv_data->battery_level, adv_data->active_layer);

    // PRIORITY: Find existing keyboard by BLE address first (unique per device)
    // This fixes the same-name keyboard conflict issue
    int index = find_keyboard_by_ble_addr(addr);
    bool is_new = false;

    if (index < 0) {
        // Fallback: Try to find by ID and role (for backward compatibility)
        index = find_keyboard_by_id_and_role(keyboard_id, adv_data->device_role);
    }

    if (index < 0) {
        // Find empty slot for new keyboard
        index = find_empty_slot();
        if (index < 0) {
            LOG_WRN("No empty slots for new keyboard");
            scanner_unlock();
            return;
        }
        is_new = true;
        LOG_INF("Creating NEW slot %d for %s (%s) BLE=%02X:%02X:%02X:%02X:%02X:%02X",
               index, role_str, device_name,
               addr->a.val[5], addr->a.val[4], addr->a.val[3],
               addr->a.val[2], addr->a.val[1], addr->a.val[0]);
    }

    // High-priority change detection (before updating data)
    // Layer, modifier, profile changes trigger immediate display update
    bool high_priority_change = is_new ||
        (keyboards[index].data.active_layer != adv_data->active_layer) ||
        (keyboards[index].data.modifier_flags != adv_data->modifier_flags) ||
        (keyboards[index].data.profile_slot != adv_data->profile_slot);

    // Update keyboard status
    keyboards[index].active = true;
    keyboards[index].last_seen = now;
    keyboards[index].rssi = rssi;
    memcpy(&keyboards[index].data, adv_data, sizeof(struct zmk_status_adv_data));

    // Store BLE address for unique identification
    memcpy(keyboards[index].ble_addr, addr->a.val, 6);
    keyboards[index].ble_addr_type = addr->type;

    // Detect v2 keyboard (Periodic Advertising support) from HAS_PERIODIC flag
    // Debug: Log status_flags value to diagnose v2 detection issues
    LOG_DBG("📊 Keyboard status_flags=0x%02X (HAS_PERIODIC bit=%d)",
            adv_data->status_flags, (adv_data->status_flags & ZMK_STATUS_FLAG_HAS_PERIODIC) ? 1 : 0);

    bool has_periodic = (adv_data->status_flags & ZMK_STATUS_FLAG_HAS_PERIODIC) != 0;
    if (has_periodic != keyboards[index].has_periodic) {
        keyboards[index].has_periodic = has_periodic;
        LOG_INF("🔄 Keyboard %s supports Periodic ADV: %s (flags=0x%02X)",
                keyboards[index].ble_name[0] ? keyboards[index].ble_name : "Unknown",
                has_periodic ? "YES (v2)" : "NO (v1)",
                adv_data->status_flags);
    }

    // Check SID cache for this address (populated by Extended Scanning)
    // The periodic ADV set uses same identity address as legacy ADV
#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
    if (has_periodic) {
        int cached_sid = get_cached_sid(addr);
        if (cached_sid >= 0 && keyboards[index].sid != (uint8_t)cached_sid) {
            LOG_INF("📡 Keyboard %d SID from cache: %d -> %d",
                    index, keyboards[index].sid, cached_sid);
            keyboards[index].sid = (uint8_t)cached_sid;
        }
    }
#endif

    // Update name: always set if empty, otherwise only update if real name (not "Unknown")
    if (keyboards[index].ble_name[0] == '\0') {
        // First time - set whatever we have (even "Unknown")
        strncpy(keyboards[index].ble_name, device_name, sizeof(keyboards[index].ble_name) - 1);
        keyboards[index].ble_name[sizeof(keyboards[index].ble_name) - 1] = '\0';
    } else if (strcmp(device_name, "Unknown") != 0 &&
               strncmp(keyboards[index].ble_name, device_name, sizeof(keyboards[index].ble_name)) != 0) {
        // Real name received - update from "Unknown" to actual name
        strncpy(keyboards[index].ble_name, device_name, sizeof(keyboards[index].ble_name) - 1);
        keyboards[index].ble_name[sizeof(keyboards[index].ble_name) - 1] = '\0';
        LOG_INF("Updated keyboard name: %s (slot %d)", device_name, index);
    }
    
    // Debug: Print current active slots (at DBG level to reduce spam)
    if (is_new) {
        LOG_DBG("Current active slots:");
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            if (keyboards[i].active) {
                uint32_t id = get_keyboard_id_from_data(&keyboards[i].data);
                const char *role = (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" :
                                  (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE";
                LOG_DBG("SLOT %d: %s (%s) ID=%08X Battery=%d%%",
                       i, role, keyboards[i].ble_name, id, keyboards[i].data.battery_level);
            }
        }
    }
    
    // Release mutex
    scanner_unlock();

    // Trigger high-priority display update for layer/modifier/profile changes
    // This schedules a work handler with 10ms delay - safe to call from BLE callback
    if (high_priority_change) {
        scanner_trigger_high_priority_update();
        LOG_DBG("⚡ High priority change: layer=%d mod=0x%02x profile=%d",
                adv_data->active_layer, adv_data->modifier_flags, adv_data->profile_slot);
    }

    // Log for debugging (low-priority updates still handled by 1Hz periodic cycle)
    if (is_new) {
        const char *role_str = "UNKNOWN";
        if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";

        LOG_INF("New %s device found: %s (slot %d)", role_str, device_name, index);
    }
}

static void process_advertisement(const struct zmk_status_adv_data *adv_data, int8_t rssi) {
    uint32_t keyboard_id = get_keyboard_id_from_data(adv_data);
    uint32_t now = k_uptime_get_32();
    
    // Debug: Print what we received (at DBG level to reduce spam)
    const char *role_str = "UNKNOWN";
    if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
    else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";

    LOG_DBG("Received %s, ID=%08X, Battery=%d%%, Layer=%d",
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
        LOG_DBG("Creating NEW slot %d for %s ID=%08X",
               index, role_str, keyboard_id);
    }

    // Check if data actually changed (to avoid duplicate update events)
    bool actual_data_change = false;
    if (!is_new) {
        // Compare key fields to detect actual changes
        actual_data_change = (keyboards[index].data.battery_level != adv_data->battery_level) ||
                            (keyboards[index].data.active_layer != adv_data->active_layer) ||
                            (keyboards[index].data.wpm_value != adv_data->wpm_value) ||
                            (keyboards[index].data.modifier_flags != adv_data->modifier_flags) ||
                            (keyboards[index].data.profile_slot != adv_data->profile_slot);
    }
    
    // Update keyboard status
    keyboards[index].active = true;
    keyboards[index].last_seen = now;
    keyboards[index].rssi = rssi;
    memcpy(&keyboards[index].data, adv_data, sizeof(struct zmk_status_adv_data));

    // Note: device name is updated in process_advertisement_with_name() which has access to BLE address

    // Debug: Print current active slots (at DBG level to reduce spam)
    if (is_new) {
        LOG_DBG("Current active slots:");
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            if (keyboards[i].active) {
                uint32_t id = get_keyboard_id_from_data(&keyboards[i].data);
                const char *role = (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_CENTRAL) ? "CENTRAL" :
                                  (keyboards[i].data.device_role == ZMK_DEVICE_ROLE_PERIPHERAL) ? "PERIPHERAL" : "STANDALONE";
                LOG_DBG("SLOT %d: %s ID=%08X Battery=%d%%",
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

        LOG_INF("New %s device found: %s (slot %d)", role_str, adv_data->layer_name, index);
        notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_FOUND, index);
    } else if (actual_data_change) {
        // Only notify if data actually changed
        const char *role_str = "UNKNOWN";
        if (adv_data->device_role == ZMK_DEVICE_ROLE_CENTRAL) role_str = "CENTRAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_PERIPHERAL) role_str = "PERIPHERAL";
        else if (adv_data->device_role == ZMK_DEVICE_ROLE_STANDALONE) role_str = "STANDALONE";

        LOG_DBG("%s device updated: %s, battery: %d%%",
               role_str, adv_data->layer_name, adv_data->battery_level);
        notify_event(ZMK_STATUS_SCANNER_EVENT_KEYBOARD_UPDATED, index);
    } else {
        // Data unchanged - just update last_seen timestamp silently
        LOG_DBG("Heartbeat from %s (no data change)", adv_data->layer_name);
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

    // Also update keyboard entry directly if we have one with this BLE address
    // This fixes the issue where SCAN_RSP (with name) arrives after ADV (with data)
    // and the keyboard already has "Unknown" as its name
    if (strcmp(name, "Unknown") != 0) {
        int kb_index = find_keyboard_by_ble_addr(addr);
        if (kb_index >= 0 && keyboards[kb_index].active) {
            // Only update if current name is empty or "Unknown"
            if (keyboards[kb_index].ble_name[0] == '\0' ||
                strcmp(keyboards[kb_index].ble_name, "Unknown") == 0) {
                strncpy(keyboards[kb_index].ble_name, name, sizeof(keyboards[kb_index].ble_name) - 1);
                keyboards[kb_index].ble_name[sizeof(keyboards[kb_index].ble_name) - 1] = '\0';
                LOG_INF("Updated keyboard name from SCAN_RSP: %s (slot %d)", name, kb_index);
            }
        }

        // Also update scanner_stub's local array (for main screen display)
        scanner_update_keyboard_name_by_addr(addr->a.val, name);
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
    // Return Unknown if device name not yet received
    // This occurs when manufacturer data is received before scan response
    return "Unknown";
}

static void scan_callback(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *buf) {
    static int scan_count = 0;
    scan_count++;
    
    // Log every 100th scan to avoid spam (or use LOG_DBG for detailed debugging)
    if (scan_count % 100 == 1) {
        LOG_INF("BLE scan #%d (RSSI: %d)", scan_count, rssi);
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
            
            // Debug: Log device name (DBG level only to reduce spam)
            LOG_DBG("%s packet - %s name (len=%d): '%s'",
                   (type & BT_HCI_LE_ADV_EVT_TYPE_SCAN_RSP) ? "SCAN_RSP" : "ADV",
                   (ad_type == BT_DATA_NAME_COMPLETE) ? "COMPLETE" : "SHORTENED",
                   len, device_name);
            
            // If we received a shortened name that looks like "LalaPad", check if we can infer the full name
            if (ad_type == BT_DATA_NAME_SHORTENED && strncmp(device_name, "LalaPad", 7) == 0) {
                // This looks like a truncated "LalaPadmini" - use the full name
                strcpy(device_name, "LalaPadmini");
                printk("*** PROSPECTOR SCANNER: Expanded shortened name to: %s ***\n", device_name);
            }
            
            store_device_name(addr, device_name);
        }
        
        // Check for Prospector manufacturer data (26 bytes expected)
        if (ad_type == BT_DATA_MANUFACTURER_DATA) {
            // Early filtering: Check if this is Prospector data FIRST to reduce log spam
            if (len >= sizeof(struct zmk_status_adv_data)) {
                const struct zmk_status_adv_data *data = (const struct zmk_status_adv_data *)buf_copy.data;

                // Check if this is Prospector data: FF FF AB CD
                if (data->manufacturer_id[0] == 0xFF && data->manufacturer_id[1] == 0xFF &&
                    data->service_uuid[0] == 0xAB && data->service_uuid[1] == 0xCD) {

                    LOG_DBG("Prospector data found - Length: %d", len);

                    // Channel filtering logic - use runtime channel
                    // scanner_get_runtime_channel() is provided by system_settings_widget.c
                    // Fallback to Kconfig if settings widget not linked
                    extern uint8_t scanner_get_runtime_channel(void) __attribute__((weak));
                    uint8_t scanner_channel = 0;
                    if (scanner_get_runtime_channel) {
                        scanner_channel = scanner_get_runtime_channel();
                    }
#ifdef CONFIG_PROSPECTOR_SCANNER_CHANNEL
                    else {
                        scanner_channel = CONFIG_PROSPECTOR_SCANNER_CHANNEL;
                    }
#endif
                    uint8_t keyboard_channel = data->channel;

                    // Accept if:
                    // - Scanner channel is 0 (accept all - legacy)
                    // - Scanner channel is 10 (CHANNEL_ALL - show all keyboards)
                    // - Keyboard channel is 0 (broadcast to all scanners)
                    // - Channels match exactly
                    #define CHANNEL_ALL_VALUE 10
                    bool channel_match = (scanner_channel == 0 ||
                                        scanner_channel == CHANNEL_ALL_VALUE ||
                                        keyboard_channel == 0 ||
                                        scanner_channel == keyboard_channel);

                    if (channel_match) {
                        prospector_data = data;
                        LOG_DBG("Valid Prospector data: Ch:%d->%d Ver=%d Bat=%d%%",
                               keyboard_channel, scanner_channel, data->version, data->battery_level);
                    } else {
                        printk("*** SCANNER: Channel mismatch - KB Ch:%d, Scanner Ch:%d (filtered) ***\n",
                               keyboard_channel, scanner_channel);
                    }
                } else {
                    // Silently ignore non-Prospector devices to reduce log spam
                    LOG_DBG("Non-Prospector device: %02X%02X %02X%02X",
                           data->manufacturer_id[0], data->manufacturer_id[1],
                           data->service_uuid[0], data->service_uuid[1]);
                }
            } else {
                // Silently ignore short manufacturer data (common for other BLE devices)
                LOG_DBG("Manufacturer data too short: %d bytes", len);
            }
        }
        
        net_buf_simple_pull(&buf_copy, len);
    }
    
    // Process Prospector data if found
    if (prospector_data) {
        LOG_DBG("Central=%d%%, Peripheral=[%d,%d,%d], Layer=%d",
               prospector_data->battery_level, prospector_data->peripheral_battery[0],
               prospector_data->peripheral_battery[1], prospector_data->peripheral_battery[2],
               prospector_data->active_layer);

        // Phase 2: Send message to queue for thread-safe processing
        // Get device name for the message
        const char *device_name = get_device_name(addr);
        int msg_ret = scanner_msg_send_keyboard_data(prospector_data, rssi, device_name,
                                                     addr->a.val, addr->type);
        if (msg_ret != 0) {
            LOG_DBG("Message queue full, falling back to direct processing");
        }

        // TRANSITIONAL: Also process directly for backward compatibility
        // This will be removed once message processing is implemented in main task
        process_advertisement_with_name(prospector_data, rssi, addr);
    }
}

static void timeout_work_handler(struct k_work *work) {
    // Skip if timeout is disabled (0)
    if (KEYBOARD_TIMEOUT_MS == 0) {
        return;
    }

    // Phase 4: Send timeout check message to queue
    // This allows the main task to handle timeout processing
    int msg_ret = scanner_msg_send_timeout_check();
    if (msg_ret != 0) {
        LOG_DBG("Timeout message queue full, processing directly");
    }

    // TRANSITIONAL: Also process directly for backward compatibility
    // This will be removed once message processing handles timeout

    // Acquire mutex for keyboard array access
    if (scanner_lock(K_MSEC(50)) != 0) {
        LOG_DBG("Timeout check skipped - mutex busy");
        // Reschedule and try again
        if (scanning) {
            k_work_schedule(&timeout_work, K_MSEC(100));
        }
        return;
    }

    uint32_t now = k_uptime_get_32();

    LOG_DBG("Timeout check at time %u", now);

    // Collect timeout events first, then release mutex before notifying
    int timeout_indices[ZMK_STATUS_SCANNER_MAX_KEYBOARDS];
    int timeout_count = 0;

    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            uint32_t age = now - keyboards[i].last_seen;
            LOG_DBG("Slot %d age: %ums (timeout at %ums)",
                   i, age, KEYBOARD_TIMEOUT_MS);

            if (age > KEYBOARD_TIMEOUT_MS) {
                LOG_INF("Keyboard timeout: %s (slot %d)", keyboards[i].data.layer_name, i);
                keyboards[i].active = false;
                timeout_indices[timeout_count++] = i;
            }
        }
    }

    scanner_unlock();

    // Phase 5: Do NOT call notify_event from timeout work context
    // Display updates are handled by periodic rx_periodic_work instead
    for (int i = 0; i < timeout_count; i++) {
        LOG_INF("Keyboard lost (slot %d) - display will update on next periodic cycle",
                timeout_indices[i]);
    }

    // Reschedule timeout check (only if timeout is enabled)
    if (scanning && KEYBOARD_TIMEOUT_MS > 0) {
        k_work_schedule(&timeout_work, K_MSEC(KEYBOARD_TIMEOUT_MS / 2));
    }
}

int zmk_status_scanner_init(void) {
    // Initialize mutex first
    scanner_mutex_init();

    memset(keyboards, 0, sizeof(keyboards));
    k_work_init_delayable(&timeout_work, timeout_work_handler);

    LOG_INF("Status scanner initialized with mutex protection");
    return 0;
}

int zmk_status_scanner_start(void) {
    if (scanning) {
        return 0;
    }

#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
    // Register Extended Scan callback for SID discovery
    // This must be done BEFORE bt_le_scan_start()
    if (!extended_scan_cb_registered) {
        bt_le_scan_cb_register(&extended_scan_callbacks);
        extended_scan_cb_registered = true;
        LOG_INF("📡 Extended scan callback registered for SID discovery");
    }
#endif

    // Use 100% duty cycle for immediate response (USB powered, no battery concern)
    // interval = window = 30ms means continuous scanning with no gaps
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,  // ACTIVE to receive Scan Response packets
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_WINDOW,  // 30ms (was 60ms)
        .window = BT_GAP_SCAN_FAST_WINDOW,    // 30ms (100% duty cycle)
    };

    int err = bt_le_scan_start(&scan_param, scan_callback);
    if (err) {
        LOG_ERR("Failed to start scanning: %d", err);
        return err;
    }

    scanning = true;

    // Only schedule timeout work if timeout is enabled (non-zero)
    if (KEYBOARD_TIMEOUT_MS > 0) {
        k_work_schedule(&timeout_work, K_MSEC(KEYBOARD_TIMEOUT_MS / 2));
        LOG_INF("Status scanner started - timeout enabled (%ums)", KEYBOARD_TIMEOUT_MS);
    } else {
        LOG_INF("Status scanner started - timeout DISABLED");
    }

    LOG_INF("Status scanner started in ACTIVE mode - will receive Scan Response packets");
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

    // Note: We don't use mutex here because:
    // 1. This is a simple read operation
    // 2. The caller (update_display_from_scanner) already holds LVGL mutex
    // 3. Adding mutex here could cause deadlock with display callback
    // The keyboards array access is atomic enough for read operations

    return keyboards[index].active ? &keyboards[index] : NULL;
}

/* Get keyboard by BLE address - fixes index mismatch between scanner_stub and status_scanner */
struct zmk_keyboard_status *zmk_status_scanner_get_keyboard_by_addr(const uint8_t *ble_addr) {
    if (!ble_addr) {
        return NULL;
    }

    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active && memcmp(keyboards[i].ble_addr, ble_addr, 6) == 0) {
            return &keyboards[i];
        }
    }
    return NULL;
}

int zmk_status_scanner_get_active_count(void) {
    int count = 0;
    uint32_t now = k_uptime_get_32();

    LOG_DBG("Active keyboard check at time %u", now);

    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        if (keyboards[i].active) {
            uint32_t age = now - keyboards[i].last_seen;
            LOG_DBG("Slot %d: ACTIVE, last_seen=%u, age=%ums, name=%s",
                   i, keyboards[i].last_seen, age, keyboards[i].data.layer_name);
            count++;
        } else {
            LOG_DBG("Slot %d: INACTIVE", i);
        }
    }

    LOG_DBG("Total active count: %d", count);
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

// ============================================================================
// Extended Scanning for SID Discovery (v2.2.0)
// ============================================================================

#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)

// Extended scan callback to discover SID from periodic advertisers
// Called for ALL scan results - we filter for Extended ADV with periodic info
// NOTE: Keyboard sends TWO advertising sets:
//   1. Legacy Extended ADV (adv_set): Has Prospector data, interval=0
//   2. Periodic Extended ADV (per_adv_set): No Prospector data, interval>0, has SID
// Both use same identity address, so we cache SID by address
static void extended_scan_recv(const struct bt_le_scan_recv_info *info,
                                struct net_buf_simple *buf) {
    // Debug: Log ALL packets to see what we're receiving
    static uint32_t ext_scan_count = 0;
    ext_scan_count++;

    // Log every 100th packet to see if callback is being called
    if (ext_scan_count % 100 == 1) {
        LOG_INF("📡 EXT_SCAN[%u]: adv_type=%d, interval=%d, sid=%d, addr=%02X:%02X:..:%02X",
                ext_scan_count, info->adv_type, info->interval, info->sid,
                info->addr->a.val[5], info->addr->a.val[4], info->addr->a.val[0]);
    }

    // Only interested in Extended ADV with periodic info (interval != 0)
    if (info->interval == 0) {
        return;  // Not a periodic advertiser
    }

    // Cache the SID for this address (regardless of payload content)
    // The periodic advertising set may not have Prospector data in Extended ADV,
    // but it shares the same identity address as the legacy advertising set
    LOG_INF("📡 EXT_ADV: Periodic advertiser detected! addr=%02X:%02X:%02X:%02X:%02X:%02X SID=%d, Interval=%dms",
            info->addr->a.val[5], info->addr->a.val[4], info->addr->a.val[3],
            info->addr->a.val[2], info->addr->a.val[1], info->addr->a.val[0],
            info->sid, info->interval * 5 / 4);

    // Cache the SID for later use when we receive Prospector data from legacy ADV
    cache_periodic_sid(info->addr, info->sid);

    // Also try to update existing keyboard entry if we already know this address
    int index = find_keyboard_by_ble_addr(info->addr);
    if (index >= 0 && keyboards[index].sid != info->sid) {
        LOG_INF("📡 Keyboard %d SID updated: %d -> %d",
                index, keyboards[index].sid, info->sid);
        keyboards[index].sid = info->sid;
    }
}

#endif // CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC

// ============================================================================
// Periodic Sync Implementation (v2.2.0)
// ============================================================================

#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)

// Periodic sync callbacks
static void per_adv_sync_synced_cb(struct bt_le_per_adv_sync *sync,
                                    struct bt_le_per_adv_sync_synced_info *info) {
    LOG_INF("✅ Periodic sync established! Interval: %d ms", info->interval * 125 / 100);
    current_sync_state = SYNC_STATE_SYNCED;
    sync_retry_count = 0;
    // Notify UI to update sync icon
    scanner_trigger_high_priority_update();
}

static void per_adv_sync_term_cb(struct bt_le_per_adv_sync *sync,
                                  const struct bt_le_per_adv_sync_term_info *info) {
    LOG_INF("⚠️ Periodic sync terminated (reason: %d)", info->reason);
    per_sync = NULL;

    // If we were synced, try to retry
    if (current_sync_state == SYNC_STATE_SYNCING) {
        if (sync_retry_count < SYNC_MAX_RETRIES) {
            LOG_INF("📡 Scheduling sync retry in %dms", SYNC_RETRY_DELAY_MS);
            k_work_schedule(&sync_retry_work, K_MSEC(SYNC_RETRY_DELAY_MS));
        } else {
            LOG_INF("📡 Max retries reached, falling back to Legacy");
            current_sync_state = SYNC_STATE_FALLBACK;
            sync_retry_count = 0;
        }
    } else {
        current_sync_state = SYNC_STATE_NONE;
    }

    scanner_trigger_high_priority_update();
}

static void per_adv_recv_cb(struct bt_le_per_adv_sync *sync,
                            const struct bt_le_per_adv_sync_recv_info *info,
                            struct net_buf_simple *buf) {
    // Parse Periodic ADV data - same format as Legacy
    if (buf->len >= sizeof(struct zmk_status_adv_data)) {
        // Check for manufacturer data header
        if (buf->len > 2) {
            uint8_t len = net_buf_simple_pull_u8(buf);
            uint8_t type = net_buf_simple_pull_u8(buf);

            if (type == BT_DATA_MANUFACTURER_DATA && len >= sizeof(struct zmk_status_adv_data)) {
                const struct zmk_status_adv_data *data =
                    (const struct zmk_status_adv_data *)buf->data;

                // Verify Prospector signature
                if (data->manufacturer_id[0] == 0xFF && data->manufacturer_id[1] == 0xFF &&
                    data->service_uuid[0] == 0xAB && data->service_uuid[1] == 0xCD) {

                    // Update selected keyboard with Periodic data
                    if (selected_keyboard_index >= 0 &&
                        selected_keyboard_index < ZMK_STATUS_SCANNER_MAX_KEYBOARDS) {

                        // Acquire mutex for keyboard update
                        if (scanner_lock(K_MSEC(5)) == 0) {
                            keyboards[selected_keyboard_index].last_seen = k_uptime_get_32();
                            keyboards[selected_keyboard_index].rssi = info->rssi;
                            memcpy(&keyboards[selected_keyboard_index].data, data,
                                   sizeof(struct zmk_status_adv_data));
                            scanner_unlock();

                            LOG_DBG("📡 Periodic data: Layer=%d, Bat=%d%%, RSSI=%d",
                                    data->active_layer, data->battery_level, info->rssi);

                            // Trigger high-priority update for responsive display
                            scanner_trigger_high_priority_update();
                        }
                    }
                }
            }
        }
    }
}

static struct bt_le_per_adv_sync_cb sync_callbacks = {
    .synced = per_adv_sync_synced_cb,
    .term = per_adv_sync_term_cb,
    .recv = per_adv_recv_cb,
};

static bool sync_callbacks_registered = false;

static void sync_retry_work_handler(struct k_work *work) {
    if (selected_keyboard_index >= 0) {
        sync_retry_count++;
        LOG_INF("📡 Sync retry %d/%d", sync_retry_count, SYNC_MAX_RETRIES + 1);
        start_periodic_sync(selected_keyboard_index);
    }
}

static int start_periodic_sync(int keyboard_index) {
    if (keyboard_index < 0 || keyboard_index >= ZMK_STATUS_SCANNER_MAX_KEYBOARDS) {
        return -EINVAL;
    }

    struct zmk_keyboard_status *kb = &keyboards[keyboard_index];
    if (!kb->active || !kb->has_periodic) {
        LOG_DBG("📡 Keyboard %d not eligible for Periodic sync", keyboard_index);
        current_sync_state = SYNC_STATE_NONE;
        return -ENOTSUP;
    }

    // Check if SID has been discovered from Extended Scanning
    // SID=0 might be valid, but we log a warning to help debug issues
    LOG_INF("📡 Starting Periodic sync: keyboard=%d, SID=%d, has_periodic=%d",
            keyboard_index, kb->sid, kb->has_periodic);

    // Register callbacks if not done yet
    if (!sync_callbacks_registered) {
        bt_le_per_adv_sync_cb_register(&sync_callbacks);
        sync_callbacks_registered = true;
    }

    // Stop existing sync if any
    stop_periodic_sync();

    // Build sync parameters
    bt_addr_le_t addr;
    memcpy(addr.a.val, kb->ble_addr, 6);
    addr.type = kb->ble_addr_type;

    struct bt_le_per_adv_sync_param sync_param = {
        .options = 0,
        .sid = kb->sid,  // TODO: Get actual SID from Extended ADV
        .skip = 0,
        .timeout = SYNC_TIMEOUT_MS / 10,  // In 10ms units
    };
    bt_addr_le_copy(&sync_param.addr, &addr);

    LOG_INF("📡 bt_le_per_adv_sync_create: SID=%d, addr=%02X:%02X:%02X:%02X:%02X:%02X (type=%d), timeout=%dms",
            sync_param.sid,
            addr.a.val[5], addr.a.val[4], addr.a.val[3],
            addr.a.val[2], addr.a.val[1], addr.a.val[0],
            addr.type, SYNC_TIMEOUT_MS);

    int err = bt_le_per_adv_sync_create(&sync_param, &per_sync);
    if (err) {
        LOG_ERR("❌ bt_le_per_adv_sync_create failed: err=%d, SID=%d", err, sync_param.sid);
        current_sync_state = SYNC_STATE_FALLBACK;
        return err;
    }

    current_sync_state = SYNC_STATE_SYNCING;
    LOG_INF("✅ Periodic sync create returned 0 - waiting for synced callback (SID=%d)", kb->sid);
    return 0;
}

static void stop_periodic_sync(void) {
    if (per_sync) {
        bt_le_per_adv_sync_delete(per_sync);
        per_sync = NULL;
    }
    current_sync_state = SYNC_STATE_NONE;
    sync_retry_count = 0;
    k_work_cancel_delayable(&sync_retry_work);
}

#endif // CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC

// ============================================================================
// Public API for Periodic Sync (v2.2.0)
// ============================================================================

sync_state_t zmk_status_scanner_get_sync_state(void) {
#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
    return current_sync_state;
#else
    return SYNC_STATE_NONE;
#endif
}

int zmk_status_scanner_select_keyboard(int keyboard_index) {
#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
    if (keyboard_index < 0 || keyboard_index >= ZMK_STATUS_SCANNER_MAX_KEYBOARDS) {
        return -EINVAL;
    }

    // Cancel any pending retry
    k_work_cancel_delayable(&sync_retry_work);

    // Stop existing sync
    stop_periodic_sync();

    selected_keyboard_index = keyboard_index;
    sync_retry_count = 0;

    // Check if keyboard supports Periodic
    struct zmk_keyboard_status *kb = &keyboards[keyboard_index];
    if (kb->active && kb->has_periodic) {
        LOG_INF("📡 Selected v2 keyboard %d - initiating Periodic sync", keyboard_index);
        return start_periodic_sync(keyboard_index);
    } else {
        LOG_INF("📡 Selected v1 keyboard %d - using Legacy mode", keyboard_index);
        current_sync_state = SYNC_STATE_NONE;
        return 0;
    }
#else
    // Without Periodic support, just track selection
    return 0;
#endif
}

int zmk_status_scanner_get_selected_keyboard(void) {
#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
    return selected_keyboard_index;
#else
    return zmk_status_scanner_get_primary_keyboard();
#endif
}

const char* zmk_status_scanner_get_sync_icon(sync_state_t state) {
    switch (state) {
    case SYNC_STATE_NONE:      return "   ";   // v1 or not selected
    case SYNC_STATE_SYNCING:   return ">>>";   // Sync in progress
    case SYNC_STATE_SYNCED:    return "SYN";   // Periodic synced
    case SYNC_STATE_FALLBACK:  return "LGC";   // Legacy fallback
    default:                   return "   ";
    }
}

// Initialize on system startup - use later priority to ensure BT is ready
static int scanner_init(const struct device *dev) {
    int ret = zmk_status_scanner_init();

#if IS_ENABLED(CONFIG_PROSPECTOR_SCANNER_PERIODIC_SYNC)
    k_work_init_delayable(&sync_retry_work, sync_retry_work_handler);
    LOG_INF("📡 Periodic Sync support enabled");
#endif

    return ret;
}

SYS_INIT(scanner_init, APPLICATION, 99);

#endif // CONFIG_PROSPECTOR_MODE_SCANNER