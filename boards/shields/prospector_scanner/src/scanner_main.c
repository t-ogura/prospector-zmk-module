/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "scanner_message.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Message queue definition - 16 messages, 4-byte aligned
K_MSGQ_DEFINE(scanner_msgq, sizeof(struct scanner_message), SCANNER_MSGQ_SIZE, 4);

// Statistics for monitoring queue health
static uint32_t msgs_sent = 0;
static uint32_t msgs_dropped = 0;
static uint32_t msgs_processed = 0;

// ========== Helper Functions for Sending Messages ==========

int scanner_msg_send_keyboard_data(const struct zmk_status_adv_data *adv_data,
                                   int8_t rssi,
                                   const char *device_name) {
    struct scanner_message msg = {
        .type = SCANNER_MSG_KEYBOARD_DATA,
        .timestamp = k_uptime_get_32(),
    };

    // Copy advertisement data
    memcpy(&msg.keyboard.adv_data, adv_data, sizeof(struct zmk_status_adv_data));
    msg.keyboard.rssi = rssi;

    // Copy device name (with null-termination safety)
    if (device_name) {
        strncpy(msg.keyboard.device_name, device_name, SCANNER_MSG_NAME_MAX - 1);
        msg.keyboard.device_name[SCANNER_MSG_NAME_MAX - 1] = '\0';
    } else {
        msg.keyboard.device_name[0] = '\0';
    }

    // Non-blocking send - safe from ISR/callback context
    int ret = k_msgq_put(&scanner_msgq, &msg, K_NO_WAIT);
    if (ret == 0) {
        msgs_sent++;
        LOG_DBG("📨 Keyboard data queued: %s (RSSI: %d)",
                device_name ? device_name : "unknown", rssi);
    } else {
        msgs_dropped++;
        LOG_WRN("⚠️ Message queue full - keyboard data dropped (sent=%d, dropped=%d)",
                msgs_sent, msgs_dropped);
    }

    return ret;
}

int scanner_msg_send_swipe(enum scanner_swipe_direction direction) {
    struct scanner_message msg = {
        .type = SCANNER_MSG_SWIPE_GESTURE,
        .timestamp = k_uptime_get_32(),
        .swipe.direction = direction,
    };

    int ret = k_msgq_put(&scanner_msgq, &msg, K_NO_WAIT);
    if (ret == 0) {
        msgs_sent++;
        const char *dir_names[] = {"UP", "DOWN", "LEFT", "RIGHT"};
        LOG_DBG("📨 Swipe gesture queued: %s", dir_names[direction]);
    } else {
        msgs_dropped++;
        LOG_WRN("⚠️ Message queue full - swipe gesture dropped");
    }

    return ret;
}

int scanner_msg_send_tap(int16_t x, int16_t y) {
    struct scanner_message msg = {
        .type = SCANNER_MSG_TOUCH_TAP,
        .timestamp = k_uptime_get_32(),
        .tap.x = x,
        .tap.y = y,
    };

    int ret = k_msgq_put(&scanner_msgq, &msg, K_NO_WAIT);
    if (ret == 0) {
        msgs_sent++;
        LOG_DBG("📨 Tap queued: (%d, %d)", x, y);
    } else {
        msgs_dropped++;
        LOG_WRN("⚠️ Message queue full - tap dropped");
    }

    return ret;
}

int scanner_msg_send_battery_update(void) {
    struct scanner_message msg = {
        .type = SCANNER_MSG_BATTERY_UPDATE,
        .timestamp = k_uptime_get_32(),
    };

    int ret = k_msgq_put(&scanner_msgq, &msg, K_NO_WAIT);
    if (ret == 0) {
        msgs_sent++;
        LOG_DBG("📨 Battery update request queued");
    } else {
        msgs_dropped++;
        LOG_WRN("⚠️ Message queue full - battery update dropped");
    }

    return ret;
}

int scanner_msg_send_timeout_check(void) {
    struct scanner_message msg = {
        .type = SCANNER_MSG_KEYBOARD_TIMEOUT,
        .timestamp = k_uptime_get_32(),
    };

    int ret = k_msgq_put(&scanner_msgq, &msg, K_NO_WAIT);
    if (ret == 0) {
        msgs_sent++;
        LOG_DBG("📨 Keyboard timeout check queued");
    } else {
        msgs_dropped++;
        LOG_WRN("⚠️ Message queue full - timeout check dropped");
    }

    return ret;
}

int scanner_msg_send_display_refresh(void) {
    struct scanner_message msg = {
        .type = SCANNER_MSG_DISPLAY_REFRESH,
        .timestamp = k_uptime_get_32(),
    };

    int ret = k_msgq_put(&scanner_msgq, &msg, K_NO_WAIT);
    if (ret == 0) {
        msgs_sent++;
        LOG_DBG("📨 Display refresh request queued");
    } else {
        msgs_dropped++;
        LOG_WRN("⚠️ Message queue full - display refresh dropped");
    }

    return ret;
}

// ========== Queue Statistics ==========

void scanner_msg_get_stats(uint32_t *sent, uint32_t *dropped, uint32_t *processed) {
    if (sent) *sent = msgs_sent;
    if (dropped) *dropped = msgs_dropped;
    if (processed) *processed = msgs_processed;
}

void scanner_msg_increment_processed(void) {
    msgs_processed++;
}

uint32_t scanner_msg_get_queue_count(void) {
    return k_msgq_num_used_get(&scanner_msgq);
}

// ========== Message Queue Access ==========

int scanner_msg_get(struct scanner_message *msg, k_timeout_t timeout) {
    return k_msgq_get(&scanner_msgq, msg, timeout);
}

void scanner_msg_purge(void) {
    k_msgq_purge(&scanner_msgq);
    LOG_INF("🗑️ Message queue purged");
}
