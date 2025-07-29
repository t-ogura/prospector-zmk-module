/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/pm/device.h>
#include <zmk/status_scanner.h>
#include <lvgl.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

// Power states
enum scanner_power_state {
    SCANNER_STATE_ACTIVE,     // Normal operation
    SCANNER_STATE_IDLE,       // Dimmed display, reduced scan rate
    SCANNER_STATE_STANDBY,    // Display off, minimal scanning
    SCANNER_STATE_SLEEP       // Deep sleep, no scanning
};

// Timeouts in milliseconds
#define IDLE_TIMEOUT_MS     30000    // 30 seconds to idle
#define STANDBY_TIMEOUT_MS  120000   // 2 minutes to standby
#define SLEEP_TIMEOUT_MS    300000   // 5 minutes to sleep

static enum scanner_power_state current_state = SCANNER_STATE_ACTIVE;
static uint32_t last_activity_time = 0;
static struct k_work_delayable power_state_work;

// External functions
extern int zmk_status_scanner_set_scan_interval(uint32_t interval_ms);
extern int zmk_display_set_brightness(uint8_t brightness);

// Forward declarations
static void update_power_state(enum scanner_power_state new_state);

// Check if any keyboards are active
static bool any_keyboards_active(void) {
    int active_count = zmk_status_scanner_get_active_count();
    return active_count > 0;
}

// Update activity timestamp
void scanner_power_mgmt_activity(void) {
    last_activity_time = k_uptime_get_32();
    
    // If we're not in active state, wake up
    if (current_state != SCANNER_STATE_ACTIVE) {
        update_power_state(SCANNER_STATE_ACTIVE);
    }
}

// Power state transition handler
static void update_power_state(enum scanner_power_state new_state) {
    if (current_state == new_state) {
        return;
    }
    
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    
    LOG_INF("Power state transition: %d -> %d", current_state, new_state);
    
    switch (new_state) {
        case SCANNER_STATE_ACTIVE:
            // Full brightness, normal scan rate
            zmk_display_set_brightness(100);
            zmk_status_scanner_set_scan_interval(500);  // 500ms scan interval
            if (current_state == SCANNER_STATE_STANDBY || current_state == SCANNER_STATE_SLEEP) {
                // Turn on display
                display_blanking_off(display);
                // Resume scanning if it was stopped
                zmk_status_scanner_start();
            }
            break;
            
        case SCANNER_STATE_IDLE:
            // Dimmed display, reduced scan rate
            zmk_display_set_brightness(30);
            zmk_status_scanner_set_scan_interval(2000);  // 2s scan interval
            break;
            
        case SCANNER_STATE_STANDBY:
            // Display off, minimal scanning
            display_blanking_on(display);
            zmk_status_scanner_set_scan_interval(5000);  // 5s scan interval
            break;
            
        case SCANNER_STATE_SLEEP:
            // Deep sleep, stop scanning
            display_blanking_on(display);
            zmk_status_scanner_stop();
            break;
    }
    
    current_state = new_state;
}

// Periodic power state check
static void power_state_work_handler(struct k_work *work) {
    uint32_t now = k_uptime_get_32();
    uint32_t idle_time = now - last_activity_time;
    bool keyboards_active = any_keyboards_active();
    
    // Determine target state based on idle time and keyboard presence
    enum scanner_power_state target_state = current_state;
    
    if (!keyboards_active && idle_time > SLEEP_TIMEOUT_MS) {
        // No keyboards and long idle -> deep sleep
        target_state = SCANNER_STATE_SLEEP;
    } else if (!keyboards_active && idle_time > STANDBY_TIMEOUT_MS) {
        // No keyboards and medium idle -> standby
        target_state = SCANNER_STATE_STANDBY;
    } else if (idle_time > IDLE_TIMEOUT_MS) {
        // Any idle time over threshold -> idle mode
        target_state = SCANNER_STATE_IDLE;
    } else {
        // Recent activity -> active
        target_state = SCANNER_STATE_ACTIVE;
    }
    
    // Apply state change if needed
    if (target_state != current_state) {
        update_power_state(target_state);
    }
    
    // Schedule next check (more frequent when active)
    uint32_t next_check = (current_state == SCANNER_STATE_ACTIVE) ? 5000 : 10000;
    k_work_schedule(&power_state_work, K_MSEC(next_check));
}

// Note: Scanner event callback is registered during init but we need
// to ensure display updates also trigger activity. The main display
// update function should call scanner_power_mgmt_activity() directly.

// Initialize power management
int scanner_power_mgmt_init(void) {
    LOG_INF("Initializing scanner power management");
    
    // Initialize work item
    k_work_init_delayable(&power_state_work, power_state_work_handler);
    
    // Set initial activity time
    last_activity_time = k_uptime_get_32();
    
    // Start periodic power state checks
    k_work_schedule(&power_state_work, K_SECONDS(5));
    
    LOG_INF("Scanner power management initialized");
    return 0;
}

// Late init to ensure scanner is ready
SYS_INIT(scanner_power_mgmt_init, APPLICATION, 90);

// Public API for manual state control
void scanner_power_mgmt_set_state(enum scanner_power_state state) {
    update_power_state(state);
}

enum scanner_power_state scanner_power_mgmt_get_state(void) {
    return current_state;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY