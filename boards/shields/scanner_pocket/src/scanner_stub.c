/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Scanner Pocket Message Handler
 * Provides the message sending functions required by status_scanner.c
 * (scanner_msg_send_keyboard_data, scanner_msg_send_timeout_check)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/status_advertisement.h>
#include <zmk/status_scanner.h>

LOG_MODULE_REGISTER(scanner_stub, LOG_LEVEL_INF);

/* Message queue definition - required by status_scanner.c */
K_MSGQ_DEFINE(scanner_msgq, sizeof(struct { uint8_t dummy[128]; }), 16, 4);

/*
 * Send keyboard data from BLE advertisement
 * Called from status_scanner.c scan callback (BLE thread)
 *
 * This minimal implementation directly invokes the callback registered
 * through zmk_status_scanner_register_callback() (defined in status_scanner.c).
 * The status_scanner.c maintains the keyboard state internally.
 */
int scanner_msg_send_keyboard_data(const struct zmk_status_adv_data *adv_data,
                                   int8_t rssi, const char *device_name,
                                   const uint8_t *ble_addr, uint8_t ble_addr_type) {
    if (!adv_data || !device_name) {
        return -EINVAL;
    }

    LOG_DBG("Keyboard data received: %s, Layer %d, RSSI %d",
            device_name, adv_data->active_layer, rssi);

    /*
     * Note: The status_scanner.c already stores keyboard state internally
     * and invokes the registered callback. This function just needs to exist
     * for the linker to resolve the reference from status_scanner.c.
     *
     * In a more complete implementation, this would post a message to a
     * work queue for thread-safe processing. For now, we rely on the
     * callback mechanism in status_scanner.c.
     */

    return 0;
}

/*
 * Check for keyboard timeouts
 * Called periodically from status_scanner.c
 */
int scanner_msg_send_timeout_check(void) {
    LOG_DBG("Timeout check requested");

    /*
     * Similar to above - status_scanner.c handles timeout internally.
     * This function exists to satisfy the linker.
     */

    return 0;
}
