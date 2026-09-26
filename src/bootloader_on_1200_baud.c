/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Reboot into the UF2 bootloader when the host sets a CDC ACM port to
 * 1200 baud (the Arduino-style "1200 baud touch"), so a scanner can be
 * reflashed from the host without a reset double-tap.
 *
 * - bootmode_set() + SYS_REBOOT_WARM, never sys_reboot(0x57): on nRF52
 *   with Zephyr >= 3.6 the reboot type is ignored (the Cortex-M
 *   sys_arch_reboot() is a bare NVIC_SystemReset(), and Zephyr 3.6
 *   removed the code behind NRF_STORE_REBOOT_TYPE_GPREGRET; ZMK still
 *   declares the symbol, but nothing reads it), so only the retained
 *   boot mode reaches the bootloader.
 * - The callback runs on the USB device work queue inside the
 *   SET_LINE_CODING data stage; the status stage is sent only after it
 *   returns. The reboot is deferred so the host's request completes, and
 *   the callback must stay short (small work queue stack).
 * - The boot mode is written before the delay, so any reset in between
 *   (e.g. the task watchdog) also lands in the bootloader.
 * - The class driver calls back only when the rate changes, and resets
 *   the rate to 115200 on every USB reset: a host retry must set another
 *   rate first, then 1200.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(bootloader_on_1200_baud, LOG_LEVEL_INF);

#define TOUCH_BAUD 1200
#define REBOOT_DELAY_MS 250

static void reboot_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    sys_reboot(SYS_REBOOT_WARM);
}

static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_work_handler);

static void dte_rate_changed(const struct device *dev, uint32_t rate) {
    if (rate != TOUCH_BAUD || k_work_delayable_is_pending(&reboot_work)) {
        return;
    }

    int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
    if (ret < 0) {
        LOG_ERR("%s: 1200 baud touch ignored, bootmode_set failed: %d", dev->name, ret);
        return;
    }

    LOG_INF("%s: 1200 baud touch, rebooting into the bootloader", dev->name);
    k_work_schedule(&reboot_work, K_MSEC(REBOOT_DELAY_MS));
}

/* Every instance, not one node label: shields and boards each add their own
 * CDC ACM node, and the host may open any of the resulting ports. */
#define REGISTER_RATE_CALLBACK(node_id)                                                  \
    do {                                                                                 \
        const struct device *dev = DEVICE_DT_GET(node_id);                               \
        if (!device_is_ready(dev)) {                                                     \
            LOG_WRN("%s not ready, no 1200 baud touch on it", dev->name);                \
        } else if (cdc_acm_dte_rate_callback_set(dev, dte_rate_changed) < 0) {           \
            LOG_WRN("%s rejected the rate callback", dev->name);                         \
        }                                                                                \
    } while (0);

static int bootloader_on_1200_baud_init(void) {
    DT_FOREACH_STATUS_OKAY(zephyr_cdc_acm_uart, REGISTER_RATE_CALLBACK)
    return 0;
}

SYS_INIT(bootloader_on_1200_baud_init, APPLICATION, 50);
