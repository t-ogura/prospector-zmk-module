/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Brightness Control DISABLED for v1.1.2 Safety
 * - All brightness control features temporarily disabled
 * - Prevents Device Tree linking issues
 * - Restores v1.1.1 stability
 * - Will be re-enabled in future version with better implementation
 */

// BRIGHTNESS CONTROL COMPLETELY DISABLED
// This file is compiled but all functions are no-ops
// Prevents linking errors while maintaining build compatibility

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// ========================================================================
// BRIGHTNESS CONTROL COMPLETELY DISABLED FOR v1.1.2
// ========================================================================
// All brightness functionality is disabled to prevent Device Tree issues
// Future versions will implement this safely

static int brightness_control_init(void) {
    LOG_INF("⚠️  Brightness Control: DISABLED in v1.1.2 for stability");
    LOG_INF("✅ Display will use hardware default brightness");
    LOG_INF("📝 Brightness control will return in future update");
    return 0;  // Always succeed
}

SYS_INIT(brightness_control_init, APPLICATION, 99);

// All brightness control code disabled to prevent boot issues
// See v1.1.1 release notes for details
