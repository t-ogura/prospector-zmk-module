/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * BLE Scanner - Receives keyboard advertisements and pushes to ring buffer.
 *
 * Architecture:
 *   BT RX thread → scan_callback() → parse ADV → scanner_msg_send_keyboard_data()
 *   LVGL timer   → scanner_process_incoming() (in scanner_stub.c) → keyboards[] → widget
 *
 * This file does NOT touch keyboards[] or any display state.
 * All keyboard state management is in scanner_stub.c (LVGL timer context only).
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

// Scanner stub functions for lock-free ring buffer push
#include "../boards/shields/prospector_scanner/src/scanner_stub.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER)

static bool scanning = false;

/* ========== Temporary Device Name Cache ========== */
/* Correlates BLE address with device name from SCAN_RSP packets.
 * Only used within scan_callback context (BT RX thread). */

static struct {
    bt_addr_le_t addr;
    char name[32];
    uint32_t timestamp;
} temp_device_names[5];

static void store_device_name(const bt_addr_le_t *addr, const char *name) {
    uint32_t now = k_uptime_get_32();
    int oldest_idx = 0;
    uint32_t oldest_time = UINT32_MAX;

    for (int i = 0; i < 5; i++) {
        /* Exact match: update existing entry */
        if (bt_addr_le_cmp(&temp_device_names[i].addr, addr) == 0) {
            strncpy(temp_device_names[i].name, name, sizeof(temp_device_names[i].name) - 1);
            temp_device_names[i].name[sizeof(temp_device_names[i].name) - 1] = '\0';
            temp_device_names[i].timestamp = now;
            return;
        }
        /* Track oldest entry for LRU eviction */
        if (temp_device_names[i].timestamp < oldest_time) {
            oldest_time = temp_device_names[i].timestamp;
            oldest_idx = i;
        }
    }

    /* No match found: evict oldest entry (LRU) */
    bt_addr_le_copy(&temp_device_names[oldest_idx].addr, addr);
    strncpy(temp_device_names[oldest_idx].name, name, sizeof(temp_device_names[oldest_idx].name) - 1);
    temp_device_names[oldest_idx].name[sizeof(temp_device_names[oldest_idx].name) - 1] = '\0';
    temp_device_names[oldest_idx].timestamp = now;
}

static const char *get_device_name(const bt_addr_le_t *addr) {
    for (int i = 0; i < 5; i++) {
        if (bt_addr_le_cmp(&temp_device_names[i].addr, addr) == 0) {
            return temp_device_names[i].name;
        }
    }
    return "Unknown";
}

/* ========== BLE Scan Callback ========== */
/* Runs in BT RX thread. Parses ADV packets, pushes to ring buffer. */

static void scan_callback(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *buf) {
    static int scan_count = 0;
    scan_count++;

    if (scan_count % 100 == 1) {
        LOG_INF("BLE scan #%d (RSSI: %d)", scan_count, rssi);
    }

    if (!scanning) {
        return;
    }

    const struct zmk_status_adv_data *prospector_data = NULL;

    /* Parse advertisement data to extract both name and Prospector data */
    struct net_buf_simple buf_copy = *buf;
    while (buf_copy.len > 1) {
        uint8_t len = net_buf_simple_pull_u8(&buf_copy);
        if (len == 0 || len > buf_copy.len) {
            break;
        }

        uint8_t ad_type = net_buf_simple_pull_u8(&buf_copy);
        len--;

        /* Extract device name */
        if ((ad_type == BT_DATA_NAME_COMPLETE || ad_type == BT_DATA_NAME_SHORTENED) && len > 0) {
            char device_name[32];
            memcpy(device_name, buf_copy.data, MIN(len, sizeof(device_name) - 1));
            device_name[MIN(len, sizeof(device_name) - 1)] = '\0';

            LOG_DBG("%s packet - %s name (len=%d): '%s'",
                   (type & BT_HCI_LE_ADV_EVT_TYPE_SCAN_RSP) ? "SCAN_RSP" : "ADV",
                   (ad_type == BT_DATA_NAME_COMPLETE) ? "COMPLETE" : "SHORTENED",
                   len, device_name);

            if (ad_type == BT_DATA_NAME_SHORTENED && strncmp(device_name, "LalaPad", 7) == 0) {
                strcpy(device_name, "LalaPadmini");
                LOG_INF("Expanded shortened name to: %s", device_name);
            }

            store_device_name(addr, device_name);
        }

        /* Check for Prospector manufacturer data */
        if (ad_type == BT_DATA_MANUFACTURER_DATA) {
            if (len >= sizeof(struct zmk_status_adv_data)) {
                const struct zmk_status_adv_data *data = (const struct zmk_status_adv_data *)buf_copy.data;

                if (data->manufacturer_id[0] == 0xFF && data->manufacturer_id[1] == 0xFF &&
                    data->service_uuid[0] == 0xAB && data->service_uuid[1] == 0xCD) {

                    LOG_DBG("Prospector data found - Length: %d", len);

                    /* Channel filtering */
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

                    bool channel_match = (scanner_channel == 0 ||
                                        scanner_channel >= 10 ||
                                        keyboard_channel == 0 ||
                                        scanner_channel == keyboard_channel);

                    if (channel_match) {
                        prospector_data = data;
                        LOG_DBG("Valid Prospector data: Ch:%d->%d Ver=%d Bat=%d%%",
                               keyboard_channel, scanner_channel, data->version, data->battery_level);
                    } else {
                        LOG_DBG("Channel mismatch - KB Ch:%d, Scanner Ch:%d (filtered)",
                                keyboard_channel, scanner_channel);
                    }
                } else {
                    LOG_DBG("Non-Prospector device: %02X%02X %02X%02X",
                           data->manufacturer_id[0], data->manufacturer_id[1],
                           data->service_uuid[0], data->service_uuid[1]);
                }
            } else {
                LOG_DBG("Manufacturer data too short: %d bytes", len);
            }
        }

        net_buf_simple_pull(&buf_copy, len);
    }

    /* Push to ring buffer for LVGL timer to process */
    if (prospector_data) {
        LOG_DBG("Central=%d%%, Peripheral=[%d,%d,%d], Layer=%d",
               prospector_data->battery_level, prospector_data->peripheral_battery[0],
               prospector_data->peripheral_battery[1], prospector_data->peripheral_battery[2],
               prospector_data->active_layer);

        const char *device_name = get_device_name(addr);
        int ret = scanner_msg_send_keyboard_data(prospector_data, rssi, device_name,
                                                  addr->a.val, addr->type);
        if (ret != 0) {
            LOG_DBG("Ring buffer full, advertisement dropped");
        }
    }
}

/* ========== Public API ========== */
/* All keyboard state queries delegate to scanner_stub.c (single source of truth) */

int zmk_status_scanner_init(void) {
    LOG_INF("Status scanner initialized (lock-free architecture)");
    return 0;
}

int zmk_status_scanner_start(void) {
    if (scanning) {
        return 0;
    }

    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_WINDOW,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };

    int err = bt_le_scan_start(&scan_param, scan_callback);
    if (err) {
        LOG_ERR("Failed to start scanning: %d", err);
        return err;
    }

    scanning = true;
    LOG_INF("Status scanner started (ACTIVE mode, 100%% duty cycle)");
    return 0;
}

int zmk_status_scanner_stop(void) {
    if (!scanning) {
        return 0;
    }

    scanning = false;

    int err = bt_le_scan_stop();
    if (err) {
        LOG_ERR("Failed to stop scanning: %d", err);
        return err;
    }

    LOG_INF("Status scanner stopped");
    return 0;
}

int zmk_status_scanner_register_callback(zmk_status_scanner_callback_t callback) {
    /* No-op: callbacks removed in lock-free architecture.
     * Display updates via pending_data in scanner_stub.c. */
    ARG_UNUSED(callback);
    return 0;
}

struct zmk_keyboard_status *zmk_status_scanner_get_keyboard(int index) {
    return scanner_get_keyboard_status(index);
}

int zmk_status_scanner_get_active_count(void) {
    return scanner_get_active_keyboard_count();
}

int zmk_status_scanner_get_primary_keyboard(void) {
    /* Find most recently seen keyboard in scanner_stub.c's keyboards[] */
    int primary = -1;
    uint32_t latest_seen = 0;

    for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
        struct zmk_keyboard_status *kbd = scanner_get_keyboard_status(i);
        if (kbd && kbd->last_seen > latest_seen) {
            latest_seen = kbd->last_seen;
            primary = i;
        }
    }

    return primary;
}

SYS_INIT(zmk_status_scanner_init, APPLICATION, 99);

#endif /* CONFIG_PROSPECTOR_MODE_SCANNER */
