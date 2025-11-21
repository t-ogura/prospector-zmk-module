/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/status_advertisement.h>

// Maximum device name length
#define SCANNER_MSG_NAME_MAX 32

// Message types for scanner main loop
enum scanner_msg_type {
    // BLE scanning messages
    SCANNER_MSG_KEYBOARD_DATA,      // Keyboard advertisement received
    SCANNER_MSG_KEYBOARD_TIMEOUT,   // Keyboard timeout check request

    // Touch/UI messages
    SCANNER_MSG_SWIPE_GESTURE,      // Swipe gesture detected
    SCANNER_MSG_TOUCH_TAP,          // Tap detected (for keyboard selection)
    SCANNER_MSG_TIMEOUT_WAKE,       // Wake from timeout (touch detected)

    // Periodic update messages
    SCANNER_MSG_BATTERY_UPDATE,     // Battery status update request
    SCANNER_MSG_DISPLAY_REFRESH,    // Display refresh request
};

// Swipe directions (matching existing enum)
enum scanner_swipe_direction {
    SCANNER_SWIPE_UP = 0,
    SCANNER_SWIPE_DOWN,
    SCANNER_SWIPE_LEFT,
    SCANNER_SWIPE_RIGHT,
};

// Message structure for scanner main loop
struct scanner_message {
    enum scanner_msg_type type;
    uint32_t timestamp;  // k_uptime_get_32() when message was created

    union {
        // SCANNER_MSG_KEYBOARD_DATA
        struct {
            struct zmk_status_adv_data adv_data;
            int8_t rssi;
            char device_name[SCANNER_MSG_NAME_MAX];
        } keyboard;

        // SCANNER_MSG_SWIPE_GESTURE
        struct {
            enum scanner_swipe_direction direction;
        } swipe;

        // SCANNER_MSG_TOUCH_TAP
        struct {
            int16_t x;
            int16_t y;
        } tap;

        // SCANNER_MSG_KEYBOARD_TIMEOUT (no additional data)
        // SCANNER_MSG_BATTERY_UPDATE (no additional data)
        // SCANNER_MSG_DISPLAY_REFRESH (no additional data)
    };
};

// Message queue size - needs to handle burst of BLE advertisements
#define SCANNER_MSGQ_SIZE 16

// Message queue declaration (defined in scanner_main.c)
extern struct k_msgq scanner_msgq;

// Helper functions for sending messages (non-blocking, safe from any context)

/**
 * Send keyboard data message from BLE scan callback
 * @param adv_data Pointer to advertisement data
 * @param rssi Signal strength
 * @param device_name Device name (can be NULL)
 * @return 0 on success, -ENOMSG if queue full
 */
int scanner_msg_send_keyboard_data(const struct zmk_status_adv_data *adv_data,
                                   int8_t rssi,
                                   const char *device_name);

/**
 * Send swipe gesture message from touch handler
 * @param direction Swipe direction
 * @return 0 on success, -ENOMSG if queue full
 */
int scanner_msg_send_swipe(enum scanner_swipe_direction direction);

/**
 * Send tap message from touch handler
 * @param x X coordinate
 * @param y Y coordinate
 * @return 0 on success, -ENOMSG if queue full
 */
int scanner_msg_send_tap(int16_t x, int16_t y);

/**
 * Send battery update request (from timer)
 * @return 0 on success, -ENOMSG if queue full
 */
int scanner_msg_send_battery_update(void);

/**
 * Send keyboard timeout check request (from timer)
 * @return 0 on success, -ENOMSG if queue full
 */
int scanner_msg_send_timeout_check(void);

/**
 * Send display refresh request
 * @return 0 on success, -ENOMSG if queue full
 */
int scanner_msg_send_display_refresh(void);

/**
 * Send timeout wake request (from touch gesture)
 * @return 0 on success, -ENOMSG if queue full
 */
int scanner_msg_send_timeout_wake(void);

/**
 * Get message from queue (blocking with timeout)
 * @param msg Pointer to message structure to fill
 * @param timeout Timeout for waiting
 * @return 0 on success, -ENOMSG if timeout
 */
int scanner_msg_get(struct scanner_message *msg, k_timeout_t timeout);

/**
 * Purge all messages from queue
 */
void scanner_msg_purge(void);

/**
 * Get queue statistics
 * @param sent Pointer to store sent count (can be NULL)
 * @param dropped Pointer to store dropped count (can be NULL)
 * @param processed Pointer to store processed count (can be NULL)
 */
void scanner_msg_get_stats(uint32_t *sent, uint32_t *dropped, uint32_t *processed);

/**
 * Increment processed message counter (call after processing each message)
 */
void scanner_msg_increment_processed(void);

/**
 * Get current queue depth
 * @return Number of messages in queue
 */
uint32_t scanner_msg_get_queue_count(void);

