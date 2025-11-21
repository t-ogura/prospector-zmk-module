/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zmk/status_scanner.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include "scanner_battery_widget.h"
#include "connection_status_widget.h"
#include "layer_status_widget.h"
#include "modifier_status_widget.h"
#include "wpm_status_widget.h"
#include "scanner_battery_status_widget.h"
#include "scanner_message.h"  // Message queue for thread-safe architecture

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
// Touch-enabled version: swipe navigation and settings screens
#include "system_settings_widget.h"
#include "display_settings_widget.h"
#include "keyboard_list_widget.h"
#include "touch_handler.h"
#include "events/swipe_gesture_event.h"
#else
// Non-touch version: signal status widget for display-only mode
#include "signal_status_widget.h"
#endif

// Brightness control for timeout dimming
#include "brightness_control.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Backward compatibility: provide defaults for v1.1.0 configs if not defined
#ifndef CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#define CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS 10  // 10% default
#endif

#ifndef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS
#define CONFIG_PROSPECTOR_FIXED_BRIGHTNESS 60   // 60% default
#endif

// Brightness control functions removed in v1.1.1 simplification
// External brightness control is handled by brightness_control.c

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

// Global UI objects for dynamic updates
static lv_obj_t *device_name_label = NULL;
lv_obj_t *main_screen = NULL;  // Non-static for external access (touch_handler.c)
// Battery widget (DYNAMIC ALLOCATION - created when main screen shown, destroyed when switching to overlays)
static struct zmk_widget_scanner_battery *battery_widget = NULL;
// Connection widget (DYNAMIC ALLOCATION - created when main screen shown, destroyed when switching to overlays)
static struct zmk_widget_connection_status *connection_widget = NULL;
// Layer widget (DYNAMIC ALLOCATION - created when main screen shown, destroyed when switching to overlays)
static struct zmk_widget_layer_status *layer_widget = NULL;
// Modifier widget (DYNAMIC ALLOCATION - created only when modifiers are pressed)
static struct zmk_widget_modifier_status *modifier_widget = NULL;
// WPM widget (DYNAMIC ALLOCATION - created when main screen shown, destroyed when switching to overlays)
static struct zmk_widget_wpm_status *wpm_widget = NULL;

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
// Touch-enabled version: settings and navigation widgets
// System settings widget for settings screen (DYNAMIC ALLOCATION)
static struct zmk_widget_system_settings *system_settings_widget = NULL;
// Display settings widget for display configuration (DYNAMIC ALLOCATION)
static struct zmk_widget_display_settings *display_settings_widget = NULL;
// Keyboard list widget for showing active keyboards (DYNAMIC ALLOCATION)
static struct zmk_widget_keyboard_list *keyboard_list_widget = NULL;
#else
// Non-touch version: signal status widget
static struct zmk_widget_signal_status signal_widget;
#endif

// Global debug widget for sensor diagnostics - DISABLED (debug only)
// struct zmk_widget_debug_status debug_widget;

// ========== Value Cache for Dynamic Widgets ==========
// Cache last displayed values so they can be restored when widgets are recreated
static char cached_device_name[32] = "Scanning...";
static uint8_t cached_battery_level = 0;
static bool cached_battery_valid = false;
static uint8_t cached_wpm_value = 0;
static struct zmk_keyboard_status cached_keyboard_status;
static bool cached_status_valid = false;

// ========== Timeout Brightness Control ==========
// Track reception timeout state for display dimming
static uint32_t last_keyboard_reception_time = 0;
static bool timeout_dimmed = false;  // True when display is dimmed due to timeout
static uint8_t brightness_before_timeout = 0;  // Store brightness to restore after timeout

// Default timeout brightness if not configured
#ifndef CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS
#define CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS 1
#endif

#ifndef CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS
#define CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS 480000
#endif

// ========== Brightness PWM Control (Main Thread Only) ==========
// CRITICAL: All PWM access MUST happen in main thread (via message handlers)
// NEVER call led_set_brightness from work queue context!

#include <zephyr/drivers/led.h>

static const struct device *pwm_dev = NULL;
static uint8_t current_brightness = 50;  // Current actual brightness
static uint8_t target_brightness = 50;   // Target brightness for fading
static uint8_t fade_step_count = 0;
static uint8_t fade_total_steps = 10;
static bool auto_brightness_enabled = true;
static uint8_t manual_brightness_setting = 65;

// Set brightness directly (main thread only!)
static void set_pwm_brightness(uint8_t brightness) {
    if (!pwm_dev || !device_is_ready(pwm_dev)) {
        return;
    }

    if (brightness > 100) brightness = 100;
    if (brightness < 1) brightness = 1;

    int ret = led_set_brightness(pwm_dev, 0, brightness);
    if (ret < 0) {
        LOG_WRN("Failed to set PWM brightness: %d", ret);
        return;
    }

    current_brightness = brightness;
    LOG_DBG("🔆 PWM brightness set: %d%%", brightness);
}

// Start brightness fade (sets target, fade steps handled by messages)
static void start_brightness_fade(uint8_t new_target) {
    if (new_target == target_brightness) {
        return; // No change
    }

    target_brightness = new_target;
    fade_step_count = 0;
    fade_total_steps = CONFIG_PROSPECTOR_BRIGHTNESS_FADE_STEPS;

    LOG_DBG("🔄 Fade start: %d%% -> %d%% (%d steps)",
            current_brightness, target_brightness, fade_total_steps);

    // Trigger first fade step via message
    scanner_msg_send_brightness_fade_step();
}

// Execute one fade step (called from message handler)
static void execute_fade_step(void) {
    if (current_brightness == target_brightness) {
        return; // Fade complete
    }

    fade_step_count++;

    // Calculate intermediate brightness
    int brightness_diff = (int)target_brightness - (int)current_brightness;
    int step_change = (brightness_diff * fade_step_count) / fade_total_steps;
    uint8_t new_brightness = current_brightness + step_change;

    // Set brightness via PWM (main thread - safe!)
    set_pwm_brightness(new_brightness);

    // Check if fade complete
    if (fade_step_count >= fade_total_steps || new_brightness == target_brightness) {
        current_brightness = target_brightness;
        LOG_DBG("✅ Fade complete: %d%%", current_brightness);
        return;
    }

    // Schedule next fade step
    scanner_msg_send_brightness_fade_step();
}

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
// Screen state management (touch version only)
enum screen_state {
    SCREEN_MAIN,              // Main status screen
    SCREEN_SETTINGS,          // Quick Actions screen (reset, bootloader)
    SCREEN_DISPLAY_SETTINGS,  // Display Settings screen (brightness, battery, layers)
    SCREEN_KEYBOARD_LIST      // Active keyboards list screen
};

static enum screen_state current_screen = SCREEN_MAIN;

// Selected keyboard index for multi-keyboard support
// -1 means auto-select first active keyboard
static int selected_keyboard_index = -1;

// Swipe gesture cooldown to prevent rapid repeated swipes causing issues
// Increased from 200ms to 500ms to prevent memory fragmentation during rapid swipes
#define SWIPE_COOLDOWN_MS 500  // 500ms cooldown between swipes
static uint32_t last_swipe_time = 0;

// Swipe processing guard - prevents concurrent swipe handling
// This flag ensures widget create/destroy operations complete atomically
// Not static - accessible from keyboard_list_widget.c for deadlock prevention
volatile bool swipe_in_progress = false;
#else
// Non-touch version: always on main screen, no swipe processing
static const int selected_keyboard_index = 0;
volatile bool swipe_in_progress = false;  // Keep for compatibility but never set to true
#endif

// LVGL mutex for thread-safe operations
// All LVGL API calls from work queues must be protected by this mutex
static struct k_mutex lvgl_mutex;
static bool lvgl_mutex_initialized = false;

// Initialize LVGL mutex (called once during display init)
static void lvgl_mutex_init(void) {
    if (!lvgl_mutex_initialized) {
        k_mutex_init(&lvgl_mutex);
        lvgl_mutex_initialized = true;
        LOG_DBG("🔒 LVGL mutex initialized");
    }
}

// Lock LVGL mutex - returns 0 on success
static int lvgl_lock(k_timeout_t timeout) {
    if (!lvgl_mutex_initialized) {
        return -EINVAL;
    }
    return k_mutex_lock(&lvgl_mutex, timeout);
}

// Unlock LVGL mutex
static void lvgl_unlock(void) {
    if (lvgl_mutex_initialized) {
        k_mutex_unlock(&lvgl_mutex);
    }
}

// Global mutex access for other files (keyboard_list_widget.c)
int scanner_lvgl_lock(void) {
    return lvgl_lock(K_MSEC(100));
}

void scanner_lvgl_unlock(void) {
    lvgl_unlock();
}

// Getter/setter for selected keyboard index (used by keyboard_list_widget)
int zmk_scanner_get_selected_keyboard(void) {
    return selected_keyboard_index;
}

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
void zmk_scanner_set_selected_keyboard(int index) {
    if (index >= -1 && index < CONFIG_PROSPECTOR_MAX_KEYBOARDS) {
        selected_keyboard_index = index;
        LOG_INF("🎯 Selected keyboard changed to index %d", index);
    }
}
#endif

// Scanner's own battery status widget (top-right corner)
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
static struct zmk_widget_scanner_battery_status scanner_battery_widget;
#endif

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
// Battery monitoring state - global to manage start/stop properly
static bool battery_monitoring_active = false;

// Backward compatibility: provide default interval if not defined
#ifndef CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S
#define CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S 120  // 2 minutes default
#endif
#endif

// Forward declarations
static void trigger_scanner_start(void);

// Forward declaration for signal timeout checking
static void check_signal_timeout_handler(struct k_work *work);
static void periodic_rx_update_handler(struct k_work *work);

// Forward declaration for battery widget update
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
static void update_scanner_battery_widget(void);
#endif

// Work queue for periodic signal timeout checking
static K_WORK_DELAYABLE_DEFINE(signal_timeout_work, check_signal_timeout_handler);

// Work queue for 1Hz RX periodic updates
static K_WORK_DELAYABLE_DEFINE(rx_periodic_work, periodic_rx_update_handler);

// Work queue for frequent battery debug updates (every 5 seconds for visibility)
static void battery_debug_update_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(battery_debug_work, battery_debug_update_handler);

// Memory monitoring work (every 30 seconds)
static void memory_monitor_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(memory_monitor_work, memory_monitor_handler);

// Phase 2 Reconstruction: LVGL timer for message queue processing
// This runs in LVGL main thread context - safe for all LVGL operations
static lv_timer_t *main_loop_timer = NULL;
static void main_loop_timer_cb(lv_timer_t *timer);

// Forward declarations for main loop processing
static void process_keyboard_data_message(struct scanner_message *msg);
static void process_display_refresh(void);
static void process_battery_update(void);

// Display update work - processes BLE data updates in work queue context
// This is safe because work queue runs in same thread as LVGL
static void display_update_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(display_update_work, display_update_work_handler);

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
// Forward declaration for swipe processing (used by message_queue_handler)
static void process_swipe_direction(int direction);
#endif

// Periodic signal timeout check (every 5 seconds)
static void check_signal_timeout_handler(struct k_work *work) {
    // Check signal widget for timeout - DISABLED
    // zmk_widget_signal_status_check_timeout(&signal_widget);

    // Schedule next check
    k_work_schedule(&signal_timeout_work, K_SECONDS(5));
}

// Display update work handler - Phase 2: Now just sends message to LVGL timer
// This is called from rx_periodic_work (1Hz) and keyboard data processing
static void display_update_work_handler(struct k_work *work) {
    // Phase 2: Send message to LVGL timer for thread-safe processing
    // No LVGL calls here - all done in main_loop_timer_cb -> process_display_refresh
    scanner_msg_send_display_refresh();
}

// 1Hz periodic RX update - called every second for smooth rate decline
static void periodic_rx_update_handler(struct k_work *work) {
    // Schedule display update (runs in work queue - same thread as LVGL)
    k_work_schedule(&display_update_work, K_NO_WAIT);

    // Schedule next update in 1 second
    k_work_schedule(&rx_periodic_work, K_SECONDS(1));
}

// Memory monitoring handler - reports stack usage (DEBUG: 1s interval)
static void memory_monitor_handler(struct k_work *work) {
    // LVGL memory monitor doesn't work with custom allocator (LV_MEM_CUSTOM=1)
    // Instead, report system uptime and basic stats

    uint32_t uptime_sec = k_uptime_get() / 1000;
    uint32_t uptime_min = uptime_sec / 60;
    uint32_t uptime_hr = uptime_min / 60;

    LOG_INF("UPTIME: %uh %um %us - System running normally",
            uptime_hr, uptime_min % 60, uptime_sec % 60);

    // DEBUG: Schedule next check in 10 seconds (less frequent to reduce log spam)
    k_work_schedule(&memory_monitor_work, K_SECONDS(10));
}

// ========== Phase 2 Reconstruction: LVGL Timer Main Loop ==========
// This timer runs in LVGL main thread context - all LVGL operations are safe here
// No mutexes needed because we ARE the LVGL thread

static void main_loop_timer_cb(lv_timer_t *timer) {
    struct scanner_message msg;
    int processed = 0;
    const int max_per_cycle = 8;  // Process up to 8 messages per cycle to avoid blocking

    // Process available messages (non-blocking)
    // Note: swipe_in_progress check moved to individual message handlers
    while (processed < max_per_cycle &&
           scanner_msg_get(&msg, K_NO_WAIT) == 0) {

        switch (msg.type) {
            case SCANNER_MSG_KEYBOARD_DATA:
                // Process keyboard data - update keyboards[] and widgets
                // Skip during swipe to prevent widget access during destruction
                if (swipe_in_progress) {
                    LOG_DBG("📥 MQ: Keyboard data skipped - swipe in progress");
                    scanner_msg_increment_processed();
                    break;
                }
                process_keyboard_data_message(&msg);
                scanner_msg_increment_processed();
                break;

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
            case SCANNER_MSG_SWIPE_GESTURE:
                // Phase 5: Process swipe in LVGL main thread context
                // This is where all LVGL operations happen - safe from any thread issues
                // Note: swipe_in_progress check is inside process_swipe_direction
                LOG_INF("📥 MQ: Processing swipe gesture: %d", msg.swipe.direction);
                process_swipe_direction(msg.swipe.direction);
                scanner_msg_increment_processed();
                break;

            case SCANNER_MSG_TOUCH_TAP:
                // TODO Phase 3: Process tap here for keyboard selection
                LOG_DBG("📥 MQ: Tap at (%d, %d)", msg.tap.x, msg.tap.y);
                scanner_msg_increment_processed();
                break;
#endif

            case SCANNER_MSG_BATTERY_UPDATE:
                // Process battery update - safe to call LVGL here
                // Skip during swipe to prevent widget access during destruction
                if (swipe_in_progress) {
                    LOG_DBG("📥 MQ: Battery update skipped - swipe in progress");
                    scanner_msg_increment_processed();
                    break;
                }
                process_battery_update();
                scanner_msg_increment_processed();
                break;

            case SCANNER_MSG_KEYBOARD_TIMEOUT:
                // Check keyboard timeouts and update display
                // TODO: Implement timeout checking
                LOG_DBG("📥 MQ: Keyboard timeout check");
                scanner_msg_increment_processed();
                break;

            case SCANNER_MSG_DISPLAY_REFRESH:
                // Refresh display - safe to call all LVGL APIs here
                // Skip during swipe to prevent widget access during destruction
                if (swipe_in_progress) {
                    LOG_DBG("📥 MQ: Display refresh skipped - swipe in progress");
                    scanner_msg_increment_processed();
                    break;
                }
                process_display_refresh();
                scanner_msg_increment_processed();
                break;

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
            case SCANNER_MSG_TIMEOUT_WAKE:
                // Restore brightness after touch wake from timeout
                // This runs in main thread context - safe to call brightness functions
                timeout_dimmed = false;

                #if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
                    brightness_control_set_auto(true);
                    LOG_INF("🔆 Brightness restored (touch detected, auto brightness resumed)");
                #else
                    // Restore saved brightness
                    if (brightness_before_timeout > 0) {
                        start_brightness_fade(brightness_before_timeout);
                        LOG_INF("🔆 Brightness restoring to %d%% (touch detected)", brightness_before_timeout);
                    } else {
                        start_brightness_fade(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
                        LOG_INF("🔆 Brightness restoring to default %d%% (touch detected)",
                                CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
                    }
                #endif
                scanner_msg_increment_processed();
                break;
#endif

            // ========== Brightness Control Messages ==========
            case SCANNER_MSG_BRIGHTNESS_SENSOR_READ:
                // Read sensor and calculate target brightness
                // Main thread context - safe for I2C access!
                #if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
                if (auto_brightness_enabled && brightness_control_sensor_available()) {
                    uint16_t light_val = 0;
                    int ret = brightness_control_read_sensor(&light_val);

                    if (ret == 0) {
                        // Sensor read successful, calculate and set target
                        uint8_t target = brightness_control_map_light_to_brightness(light_val);
                        start_brightness_fade(target);
                        LOG_DBG("📥 MQ: Sensor read: light=%u -> brightness=%u%%", light_val, target);
                    } else if (ret != -EAGAIN) {
                        LOG_WRN("📥 MQ: Sensor read failed: %d", ret);
                    }
                }
                #endif
                scanner_msg_increment_processed();
                break;

            case SCANNER_MSG_BRIGHTNESS_SET_TARGET:
                // Set target brightness directly (from manual control)
                // Main thread context - safe to call PWM
                if (!auto_brightness_enabled) {
                    // Manual mode - always apply
                    start_brightness_fade(msg.brightness_target.target_brightness);
                    LOG_DBG("📥 MQ: Manual brightness target: %d%%", msg.brightness_target.target_brightness);
                } else {
                    LOG_DBG("📥 MQ: Brightness target ignored (auto mode active)");
                }
                scanner_msg_increment_processed();
                break;

            case SCANNER_MSG_BRIGHTNESS_FADE_STEP:
                // Execute one fade step
                // Main thread context - safe to call PWM
                execute_fade_step();
                scanner_msg_increment_processed();
                break;

            case SCANNER_MSG_BRIGHTNESS_SET_AUTO:
                // Enable/disable auto brightness
                auto_brightness_enabled = msg.brightness_auto.enabled;
                if (!auto_brightness_enabled) {
                    // Switch to manual mode
                    start_brightness_fade(manual_brightness_setting);
                    LOG_INF("📥 MQ: Auto brightness disabled, manual: %d%%", manual_brightness_setting);
                } else {
                    LOG_INF("📥 MQ: Auto brightness enabled");
                }
                scanner_msg_increment_processed();
                break;

            default:
                LOG_WRN("📥 MQ: Unknown message type: %d", msg.type);
                break;
        }

        processed++;
    }

    // 1Hz periodic updates for signal status widget (every 5 cycles = 1 second at 200ms interval)
    static int cycle_count = 0;
    cycle_count++;

#if !IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    // Non-touch version: Update signal status widget every 1 second for stable display
    if (cycle_count % 5 == 0 && !swipe_in_progress) {
        zmk_widget_signal_status_periodic_update(&signal_widget);
    }
#endif

    // Check for reception timeout and dim display (every 1 second)
    if (cycle_count % 5 == 0) {
        uint32_t timeout_ms = CONFIG_PROSPECTOR_SCANNER_TIMEOUT_MS;

        // Only check timeout if enabled (timeout > 0)
        if (timeout_ms > 0 && last_keyboard_reception_time > 0) {
            uint32_t now = k_uptime_get_32();
            uint32_t elapsed = now - last_keyboard_reception_time;

            // Check if timeout exceeded and not already dimmed
            if (elapsed >= timeout_ms && !timeout_dimmed) {
                // Save current brightness before dimming
                brightness_before_timeout = current_brightness;
                if (brightness_before_timeout == 0) {
                    brightness_before_timeout = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;
                }

                // Disable auto brightness temporarily to prevent sensor overriding timeout
                brightness_control_set_auto(false);
                auto_brightness_enabled = false;

                // Dim display to timeout brightness (main thread - safe!)
                start_brightness_fade(CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS);
                timeout_dimmed = true;

                LOG_INF("⏱️ Reception timeout (%dms) - display dimming to %d%% (auto brightness paused)",
                        elapsed, CONFIG_PROSPECTOR_SCANNER_TIMEOUT_BRIGHTNESS);
            }
        }
    }

    // Log stats periodically (every 50 cycles = 10 seconds at 200ms interval)
    if (cycle_count % 50 == 0) {
        uint32_t sent, dropped, proc;
        scanner_msg_get_stats(&sent, &dropped, &proc);
        LOG_INF("📊 MQ Stats: sent=%d, dropped=%d, processed=%d, queue=%d",
                sent, dropped, proc, scanner_msg_get_queue_count());
    }
}

// Process keyboard data from message queue
// This replaces direct update in BLE callback - now safe for LVGL
static void process_keyboard_data_message(struct scanner_message *msg) {
    if (!device_name_label || !main_screen) {
        return; // UI not ready yet
    }

    // Update reception time for timeout tracking
    last_keyboard_reception_time = k_uptime_get_32();

    // Restore brightness if we were dimmed due to timeout
    if (timeout_dimmed) {
        timeout_dimmed = false;

        // Re-enable auto brightness if sensor is available
        #if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
            brightness_control_set_auto(true);
            auto_brightness_enabled = true;
            LOG_INF("🔆 Brightness restoring (auto brightness resumed, keyboard received)");
        #else
            // Restore saved brightness (main thread - safe!)
            if (brightness_before_timeout > 0) {
                start_brightness_fade(brightness_before_timeout);
                LOG_INF("🔆 Brightness restoring to %d%% (keyboard received)", brightness_before_timeout);
            } else {
                // Restore to fixed brightness if no previous value
                start_brightness_fade(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
                LOG_INF("🔆 Brightness restoring to default %d%% (keyboard received)",
                        CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
            }
        #endif
    }

    // Only update when on main screen
#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    if (current_screen != SCREEN_MAIN) {
        return;
    }
#endif

    // Log received data
    LOG_DBG("📥 Processing keyboard: %s (RSSI %d)",
            msg->keyboard.device_name, msg->keyboard.rssi);

    // TODO: Update keyboards[] array here (Phase 2 complete implementation)
    // For now, trigger a display refresh which uses the existing update mechanism

    // Schedule display update work (temporary - will be removed in Phase 3)
    k_work_schedule(&display_update_work, K_NO_WAIT);
}

// Process display refresh - all LVGL operations safe here
static void process_display_refresh(void) {
    if (!device_name_label || !main_screen) {
        return;
    }

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    if (current_screen != SCREEN_MAIN) {
        return;
    }
#endif

    int active_count = zmk_status_scanner_get_active_count();

    if (active_count == 0) {
        lv_label_set_text(device_name_label, "Scanning...");

        if (battery_widget) {
            zmk_widget_scanner_battery_reset(battery_widget);
        }
        if (wpm_widget) {
            zmk_widget_wpm_status_reset(wpm_widget);
        }
#if !IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
        // Non-touch version: Reset signal status widget
        zmk_widget_signal_status_reset(&signal_widget);
#endif
    } else {
        // Use selected keyboard or first active
        int selected_idx = selected_keyboard_index;
        struct zmk_keyboard_status *kbd = NULL;

        if (selected_idx >= 0 && selected_idx < ZMK_STATUS_SCANNER_MAX_KEYBOARDS) {
            kbd = zmk_status_scanner_get_keyboard(selected_idx);
            if (!kbd || !kbd->active) {
                kbd = NULL;
            }
        }

        // Auto-select first active
        if (!kbd) {
            for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
                kbd = zmk_status_scanner_get_keyboard(i);
                if (kbd && kbd->active) {
#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
                    selected_keyboard_index = i;
#endif
                    break;
                }
                kbd = NULL;
            }
        }

        if (kbd) {
            // Update device name
            if (kbd->ble_name[0] != '\0') {
                lv_label_set_text(device_name_label, kbd->ble_name);
                strncpy(cached_device_name, kbd->ble_name, sizeof(cached_device_name) - 1);
            }

            // Update widgets - all safe here in LVGL thread
            if (wpm_widget) {
                zmk_widget_wpm_status_update(wpm_widget, kbd);
            }
            if (battery_widget) {
                zmk_widget_scanner_battery_update(battery_widget, kbd);
            }
            if (connection_widget) {
                zmk_widget_connection_status_update(connection_widget, kbd);
            }
            if (layer_widget) {
                zmk_widget_layer_status_update(layer_widget, kbd);
            }

#if !IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
            // Non-touch version: Update signal status widget with RSSI
            zmk_widget_signal_status_update(&signal_widget, kbd->rssi);
#endif

            // Cache for restore
            memcpy(&cached_keyboard_status, kbd, sizeof(cached_keyboard_status));
            cached_status_valid = true;
        }
    }
}

// Process battery update - all LVGL operations safe here
static void process_battery_update(void) {
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    update_scanner_battery_widget();
#endif
}

// Battery debug update handler - Phase 2: Now just sends message to LVGL timer
static void battery_debug_update_handler(struct k_work *work) {
    // Phase 2: Send message to LVGL timer for thread-safe processing
    // No LVGL calls here - all done in main_loop_timer_cb -> process_battery_update
    scanner_msg_send_battery_update();

    // Schedule next update in 5 seconds
    k_work_schedule(&battery_debug_work, K_SECONDS(5));
}

// Start periodic signal monitoring
static void start_signal_monitoring(void) {
    k_work_schedule(&signal_timeout_work, K_SECONDS(5));
    k_work_schedule(&rx_periodic_work, K_SECONDS(1));  // Start 1Hz updates
    k_work_schedule(&battery_debug_work, K_SECONDS(2)); // Start battery debug updates (2s delay)
    k_work_schedule(&memory_monitor_work, K_SECONDS(10)); // Uptime monitor (10s interval)

    // Phase 2: Start LVGL timer for message queue processing
    // This runs in LVGL main thread - safe for all LVGL operations
    if (!main_loop_timer) {
        main_loop_timer = lv_timer_create(main_loop_timer_cb, 200, NULL);  // 200ms interval (reduced from 100ms for better performance)
        LOG_INF("✅ LVGL main loop timer created (200ms interval)");
    }

    LOG_INF("Started periodic monitoring: signal timeout (5s), RX updates (1Hz), battery debug (5s), uptime (10s), LVGL timer (200ms)");
}

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
// Scanner battery event listener for updating battery widget
static void update_scanner_battery_widget(void) {
    uint8_t battery_level = 0;
    bool usb_powered = false;
    bool charging = false;
    static uint32_t update_counter = 0;
    update_counter++;

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_DEMO_MODE)
    // Demo mode: Show sample battery status for UI testing
    battery_level = 75;  // 75% battery level (yellow/orange range)
    usb_powered = true;  // Show USB connected
    charging = true;     // Show charging icon
    LOG_DBG("Scanner battery DEMO MODE: %d%% USB=%s charging=%s", 
            battery_level, usb_powered ? "yes" : "no", charging ? "yes" : "no");
#else
    // ZMK-native approach: Use ZMK's battery update function directly
    uint8_t zmk_battery_before = 0;
    uint8_t zmk_battery_after = 0;
    
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    zmk_battery_before = zmk_battery_state_of_charge();
#endif

    // Alternative approach: Manual sensor reading with ZMK-style processing
#if DT_HAS_CHOSEN(zmk_battery)
    const struct device *battery_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
    const char *update_result = "N/A";
    
    if (device_is_ready(battery_dev)) {
        LOG_INF("🔋 Manual battery reading with ZMK-style processing");
        
        // Replicate ZMK's battery update logic manually
        struct sensor_value state_of_charge;
        int ret = -1;
        
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_STATE_OF_CHARGE)
        // Try STATE_OF_CHARGE first (same as ZMK)
        ret = sensor_sample_fetch_chan(battery_dev, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE);
        if (ret == 0) {
            ret = sensor_channel_get(battery_dev, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &state_of_charge);
            if (ret == 0) {
                update_result = "SOC_MODE";
            }
        }
#elif IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING_FETCH_MODE_LITHIUM_VOLTAGE)
        // Try LITHIUM_VOLTAGE mode (same as ZMK)
        ret = sensor_sample_fetch_chan(battery_dev, SENSOR_CHAN_VOLTAGE);
        if (ret == 0) {
            struct sensor_value voltage;
            ret = sensor_channel_get(battery_dev, SENSOR_CHAN_VOLTAGE, &voltage);
            if (ret == 0) {
                // ZMK's lithium_ion_mv_to_pct conversion
                uint16_t mv = voltage.val1 * 1000 + (voltage.val2 / 1000);
                if (mv >= 4200) {
                    state_of_charge.val1 = 100;
                } else if (mv <= 3450) {
                    state_of_charge.val1 = 0;
                } else {
                    state_of_charge.val1 = mv * 2 / 15 - 459;
                }
                update_result = "VOLTAGE_MODE";
            }
        }
#endif
        
        if (ret != 0) {
            update_result = "SENSOR_FAIL";
            LOG_ERR("❌ Battery sensor reading failed: %d", ret);
        } else {
            // Successfully read battery - use direct value instead of cache
            battery_level = state_of_charge.val1;
            LOG_INF("✅ Battery reading succeeded: %d%% (direct from sensor)", battery_level);
        }
    } else {
        update_result = "NOT_READY";
        LOG_ERR("Battery device not ready");
    }
#else
    const char *update_result = "NO_DEVICE";
#endif

    // Get ZMK cache after our manual reading
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    zmk_battery_after = zmk_battery_state_of_charge();
#endif

    // Use direct sensor value if available, otherwise ZMK cache
    if (strcmp(update_result, "SOC_MODE") == 0 || strcmp(update_result, "VOLTAGE_MODE") == 0) {
        // battery_level already set from direct sensor reading
        LOG_INF("🎯 Using direct sensor reading: %d%%", battery_level);
    } else {
        // Fallback to ZMK cache
        battery_level = zmk_battery_after;
        LOG_INF("⚙️ Using ZMK cache fallback: %d%%", battery_level);
    }
    
    LOG_INF("🔍 ZMK Battery Update: Before=%d%% After=%d%% Result=%s", 
            zmk_battery_before, zmk_battery_after, update_result);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    usb_powered = zmk_usb_is_powered();
    // Simple charging detection: USB powered and battery < 100%
    charging = usb_powered && (battery_level < 100);
#endif

    // Always log battery status changes at INFO level for visibility
    LOG_INF("🔋 Scanner battery status: %d%% USB=%s charging=%s", 
            battery_level, usb_powered ? "yes" : "no", charging ? "yes" : "no");
    
    // Also output via printk for easier debugging
    printk("BATTERY: %d%% USB=%s charging=%s\\n", 
           battery_level, usb_powered ? "yes" : "no", charging ? "yes" : "no");
#endif

    // Update debug widget with detailed battery investigation (line 1)
    static char debug_text[128];
    
    // Get ZMK cached value for comparison
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    uint8_t zmk_cached = zmk_battery_state_of_charge();
#else
    uint8_t zmk_cached = 0;
#endif
    
    // ZMK-native battery debug display
    snprintf(debug_text, sizeof(debug_text), 
             "ZMK %d%%->%d%% #%d\n%s USB:%s CHG:%s", 
             zmk_battery_before, zmk_battery_after, update_counter,
             update_result, usb_powered ? "Y" : "N", charging ? "Y" : "N");
    // Debug widget re-enabled with safer ZMK-native approach
    // TEMPORARILY DISABLED: Battery debug overwriting sensor debug messages
    // if (debug_widget.debug_label) {
    //     zmk_widget_debug_status_set_text(&debug_widget, debug_text);
    // }

    zmk_widget_scanner_battery_status_update(&scanner_battery_widget, 
                                            battery_level, usb_powered, charging);
}

// Battery state changed event handler
// Phase 5: Send message instead of direct LVGL calls (thread safety)
static int scanner_battery_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        LOG_INF("🔋 Scanner battery event: %d%% (state changed)", ev->state_of_charge);
        // Phase 5: Send message to LVGL timer for thread-safe processing
        scanner_msg_send_battery_update();
        return 0;
    }

    return -ENOTSUP;
}

// USB connection state changed event handler
// Phase 5: Send message instead of direct LVGL calls (thread safety)
static int scanner_usb_listener(const zmk_event_t *eh) {
    if (as_zmk_usb_conn_state_changed(eh)) {
        LOG_DBG("Scanner USB connection state changed event received");
        // Phase 5: Send message to LVGL timer for thread-safe processing
        scanner_msg_send_battery_update();
        return 0;
    }

    return -ENOTSUP;
}

// Register event listeners for scanner battery monitoring
ZMK_LISTENER(scanner_battery, scanner_battery_listener);
ZMK_SUBSCRIPTION(scanner_battery, zmk_battery_state_changed);

ZMK_LISTENER(scanner_usb, scanner_usb_listener);
ZMK_SUBSCRIPTION(scanner_usb, zmk_usb_conn_state_changed);

// Forward declaration of work handler
static void battery_periodic_update_handler(struct k_work *work);

// Work queue definition 
static K_WORK_DELAYABLE_DEFINE(battery_periodic_work, battery_periodic_update_handler);

// Periodic battery status update work - Phase 2: Simplified to message sending
static void battery_periodic_update_handler(struct k_work *work) {
    static uint32_t periodic_counter = 0;
    periodic_counter++;

    LOG_INF("🔄 Periodic battery status update triggered (%ds interval) #%d",
            CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S, periodic_counter);

    // Phase 2: Send message to LVGL timer for thread-safe widget update
    // All LVGL operations now happen in main_loop_timer_cb -> process_battery_update
    scanner_msg_send_battery_update();

    // Schedule next update
    k_work_schedule(&battery_periodic_work, K_SECONDS(CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S));
}

// Start periodic battery monitoring - only when keyboards are active
static void start_battery_monitoring(void) {
    // Update battery status at configurable intervals for more responsive display
    k_work_schedule(&battery_periodic_work, K_SECONDS(CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S));
    LOG_INF("Started periodic battery monitoring (%ds intervals) - ACTIVE MODE", CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S);
    
    // Update debug widget to show monitoring started
    // if (debug_widget.debug_label) {
    //     zmk_widget_debug_status_set_text(&debug_widget, "BATTERY MONITORING STARTED");
    // }  // DISABLED
}

// Stop battery monitoring when keyboards become inactive
static void stop_battery_monitoring(void) {
    k_work_cancel_delayable(&battery_periodic_work);
    LOG_INF("Stopped periodic battery monitoring - INACTIVE MODE");
    
    // Update debug widget to show monitoring stopped
    // if (debug_widget.debug_label) {
    //     zmk_widget_debug_status_set_text(&debug_widget, "BATTERY MONITORING STOPPED");
    // }  // DISABLED
}
#endif // CONFIG_PROSPECTOR_BATTERY_SUPPORT

// Stop all periodic work queues (call before showing overlay screens)
static void stop_all_periodic_work(void) {
    LOG_DBG("⏸️  Stopping all periodic work queues");

    struct k_work_sync sync;

    // Stop and wait for each work queue
    k_work_cancel_delayable_sync(&signal_timeout_work, &sync);
    k_work_cancel_delayable_sync(&rx_periodic_work, &sync);
    k_work_cancel_delayable_sync(&battery_debug_work, &sync);
    k_work_cancel_delayable_sync(&memory_monitor_work, &sync);

    // Phase 5: Do NOT pause main_loop_timer - it needs to process swipe messages
    // Other messages (keyboard data, battery, display refresh) are skipped via swipe_in_progress check
    LOG_DBG("📝 LVGL main loop timer kept running for swipe processing");

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    k_work_cancel_delayable_sync(&battery_periodic_work, &sync);
#endif

    LOG_DBG("✅ All periodic work queues stopped");
}

// Resume all periodic work queues (call after returning to main screen)
static void resume_all_periodic_work(void) {
    LOG_DBG("▶️  Resuming all periodic work queues");

    k_work_schedule(&signal_timeout_work, K_SECONDS(1));
    k_work_schedule(&rx_periodic_work, K_SECONDS(1));
    k_work_schedule(&battery_debug_work, K_SECONDS(2));
    k_work_schedule(&memory_monitor_work, K_SECONDS(10));

    // Phase 5: main_loop_timer was never paused, so no need to resume
    LOG_DBG("📝 LVGL main loop timer already running");

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    k_work_schedule(&battery_periodic_work, K_SECONDS(CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S));
#endif

    LOG_DBG("✅ All periodic work queues resumed");
}

#if IS_ENABLED(CONFIG_PROSPECTOR_ADVERTISEMENT_FREQUENCY_DIM)
// Advertisement frequency monitoring for automatic brightness dimming
static uint32_t last_advertisement_time = 0;
static bool frequency_dimmed = false;

// Backward compatibility: provide defaults if not defined
#ifndef CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_BRIGHTNESS
#define CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_BRIGHTNESS 25  // 25% default
#endif

#ifndef CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_THRESHOLD_MS
#define CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_THRESHOLD_MS 2000  // 2 seconds default
#endif

static void check_advertisement_frequency(void) {
    uint32_t current_time = k_uptime_get_32();
    
    if (last_advertisement_time == 0) {
        last_advertisement_time = current_time;
        return;
    }
    
    uint32_t interval = current_time - last_advertisement_time;
    last_advertisement_time = current_time;
    
    // Check if frequency dropped below threshold (keyboard went idle)
    if (interval > CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_THRESHOLD_MS) {
        if (!frequency_dimmed) {
            LOG_INF("Advertisement frequency low (%dms interval), dimming to %d%%", 
                    interval, CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_BRIGHTNESS);
            // prospector_set_brightness(CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_BRIGHTNESS);
            frequency_dimmed = true;
        }
    } else {
        // Frequency increased (keyboard became active), restore brightness
        if (frequency_dimmed) {
            LOG_INF("Advertisement frequency restored (%dms interval), resuming normal brightness", interval);
            // prospector_resume_brightness(); // Function removed in v1.1.1
            frequency_dimmed = false;
        }
    }
}
#endif // CONFIG_PROSPECTOR_ADVERTISEMENT_FREQUENCY_DIM

// Scanner event callback for display updates
// Phase 5: Now sends message to LVGL timer instead of direct LVGL calls
static void update_display_from_scanner(struct zmk_status_scanner_event_data *event_data) {
    // Skip updates during swipe processing
    if (swipe_in_progress) {
        LOG_DBG("Scanner update skipped - swipe in progress");
        return;
    }

    LOG_DBG("Scanner event received: %d for keyboard %d", event_data->event, event_data->keyboard_index);

    // Phase 5: Send message to LVGL timer for thread-safe processing
    // All LVGL operations now happen in main_loop_timer_cb -> process_display_refresh
    scanner_msg_send_display_refresh();

    // Handle battery monitoring state changes (non-LVGL operations)
    int active_count = zmk_status_scanner_get_active_count();

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    if (active_count == 0) {
        // Stop battery monitoring when no keyboards are active
        stop_battery_monitoring();
        battery_monitoring_active = false;
    } else if (!battery_monitoring_active) {
        // Start battery monitoring when keyboards become active
        start_battery_monitoring();
        battery_monitoring_active = true;
    }
#endif

    // Log keyboard info for debugging
    if (active_count > 0) {
        int selected_idx = selected_keyboard_index;
        struct zmk_keyboard_status *kbd = NULL;

        if (selected_idx >= 0 && selected_idx < ZMK_STATUS_SCANNER_MAX_KEYBOARDS) {
            kbd = zmk_status_scanner_get_keyboard(selected_idx);
            if (!kbd || !kbd->active) {
                kbd = NULL;
            }
        }

        if (!kbd) {
            for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
                kbd = zmk_status_scanner_get_keyboard(i);
                if (kbd && kbd->active) {
#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
                    selected_keyboard_index = i;
#endif
                    LOG_INF("🎯 Auto-selected keyboard index %d", i);
                    break;
                }
                kbd = NULL;
            }
        }

        if (kbd && kbd->active) {
            LOG_DBG("Keyboard: %s, Battery %d%%, Layer: %d",
                    kbd->ble_name, kbd->data.battery_level, kbd->data.active_layer);
        }
    }
}

// Display rotation initialization (merged from display_rotate_init.c)
static int scanner_display_init(void) {
    // Use ERR level for critical init messages to ensure they're visible
    LOG_ERR("🚀 ===== SCANNER DISPLAY INIT STARTING =====");

    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display)) {
        LOG_ERR("❌ Display device not ready");
        return -EIO;
    }
    LOG_ERR("✅ Display device ready");
    
    // Set display orientation
#ifdef CONFIG_PROSPECTOR_ROTATE_DISPLAY_180
    int ret = display_set_orientation(display, DISPLAY_ORIENTATION_ROTATED_90);
#else
    int ret = display_set_orientation(display, DISPLAY_ORIENTATION_ROTATED_270);
#endif
    if (ret < 0) {
        LOG_ERR("Failed to set display orientation: %d", ret);
        return ret;
    }
    
    // Ensure display stays on and disable blanking
    ret = display_blanking_off(display);
    if (ret < 0) {
        LOG_WRN("Failed to turn off display blanking: %d", ret);
    }
    
    // Initialize PWM device for brightness control (main thread only)
    pwm_dev = NULL;
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
#endif

    if (pwm_dev && device_is_ready(pwm_dev)) {
        // Set initial brightness
        uint8_t initial_brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;
        set_pwm_brightness(initial_brightness);
        current_brightness = initial_brightness;
        target_brightness = initial_brightness;
        LOG_INF("✅ PWM brightness initialized: %d%%", initial_brightness);
    } else {
        LOG_WRN("⚠️ PWM device not ready - brightness control disabled");
    }

    // Note: Sensor-based brightness control is handled by brightness_control.c
    // It sends messages to this main thread for PWM updates

    // Add a delay to allow display to stabilize
    k_msleep(100);

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    // Initialize direct touch handler for raw coordinate debugging
    ret = touch_handler_init();
    if (ret < 0) {
        LOG_WRN("Touch handler init failed: %d (continuing anyway)", ret);
    } else {
        LOG_INF("✅ Touch handler initialized - will log raw coordinates");
    }

    // Note: LVGL input device will be registered when Settings screen is first opened
    // (dynamic allocation - only register when buttons are actually created)
#else
    LOG_INF("✅ Touch handler disabled (non-touch version)");
#endif

    LOG_INF("✅ Scanner display initialized successfully");
    return 0;
}

// Initialize display early in the boot process
SYS_INIT(scanner_display_init, APPLICATION, 60);

// Custom settings toggle behavior handles gesture now - no event listener needed

// Required function for ZMK_DISPLAY_STATUS_SCREEN_CUSTOM
// Following the working adapter pattern with simple, stable display
lv_obj_t *zmk_display_status_screen() {
    LOG_INF("🎨 ===== zmk_display_status_screen() CALLED =====");

    // Initialize LVGL mutex for thread-safe operations
    lvgl_mutex_init();

    // Set processing flag during initial screen creation (prevents swipe during init)
    swipe_in_progress = true;
    LOG_DBG("🔒 Screen init started - swipe blocked");

    LOG_INF("Step 1: Creating main screen object...");
    lv_obj_t *screen = lv_obj_create(NULL);
    main_screen = screen;  // Save reference for later use
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);
    // Disable scrolling on main screen to prevent swipe conflicts
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    LOG_INF("✅ Main screen created (scrolling disabled)");

    // Device name label - DYNAMIC ALLOCATION (created at boot and when returning to main screen)
    LOG_INF("Step 2: Creating device name label...");
    device_name_label = lv_label_create(screen);
    lv_obj_set_style_text_color(device_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_16, 0);
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 25);
    lv_label_set_text(device_name_label, cached_device_name);  // Use cached value
    LOG_INF("✅ Device name label created");

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    LOG_INF("Step 3: Init scanner battery status widget...");
    zmk_widget_scanner_battery_status_init(&scanner_battery_widget, screen);
    lv_obj_align(zmk_widget_scanner_battery_status_obj(&scanner_battery_widget), LV_ALIGN_TOP_RIGHT, 10, 0);
    // Apply initial visibility from global settings
#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    zmk_widget_scanner_battery_status_set_visible(&scanner_battery_widget,
                                                   display_settings_get_battery_visible_global());
    LOG_INF("✅ Scanner battery status widget initialized (visible=%s)",
            display_settings_get_battery_visible_global() ? "yes" : "no");
#else
    // Non-touch version: always visible (no settings screen to toggle)
    zmk_widget_scanner_battery_status_set_visible(&scanner_battery_widget, true);
    LOG_INF("✅ Scanner battery status widget initialized (always visible - non-touch)");
#endif
#endif

    // Connection widget - DYNAMIC ALLOCATION (created at boot and when returning to main screen)
    LOG_INF("Step 4: Init connection status widget...");
    connection_widget = zmk_widget_connection_status_create(screen);
    if (connection_widget) {
        lv_obj_align(zmk_widget_connection_status_obj(connection_widget), LV_ALIGN_TOP_RIGHT, -5, 45);
        // Restore cached values if available
        if (cached_status_valid) {
            zmk_widget_connection_status_update(connection_widget, &cached_keyboard_status);
        }
    }
    LOG_INF("✅ Connection status widget created");

    // Layer widget - DYNAMIC ALLOCATION (created at boot and when returning to main screen)
    LOG_INF("Step 5: Init layer status widget...");
    layer_widget = zmk_widget_layer_status_create(screen);
    if (layer_widget) {
        lv_obj_align(zmk_widget_layer_status_obj(layer_widget), LV_ALIGN_CENTER, 0, -10);
        // Restore cached values if available
        if (cached_status_valid) {
            zmk_widget_layer_status_update(layer_widget, &cached_keyboard_status);
        }
    }
    LOG_INF("✅ Layer status widget created");

    LOG_INF("Step 6: Modifier status widget (dynamic allocation - created on modifier press)");
    // Widget will be created dynamically when modifiers are pressed
    // zmk_widget_modifier_status_init(&modifier_widget, screen);  // REMOVED - now dynamic
    // lv_obj_align(zmk_widget_modifier_status_obj(&modifier_widget), LV_ALIGN_CENTER, 0, 30);
    LOG_INF("✅ Modifier status widget setup complete");

    // Battery widget - DYNAMIC ALLOCATION (created at boot and when returning to main screen)
    LOG_INF("Step 7: Init battery widget...");
    battery_widget = zmk_widget_scanner_battery_create(screen);
    if (battery_widget) {
        lv_obj_align(zmk_widget_scanner_battery_obj(battery_widget), LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_set_height(zmk_widget_scanner_battery_obj(battery_widget), 50);
        // Restore cached values if available
        if (cached_status_valid) {
            zmk_widget_scanner_battery_update(battery_widget, &cached_keyboard_status);
        }
    }
    LOG_INF("✅ Battery widget created");

    // WPM widget - DYNAMIC ALLOCATION (created at boot and when returning to main screen)
    LOG_INF("Step 8: Init WPM status widget...");
    wpm_widget = zmk_widget_wpm_status_create(screen);
    if (wpm_widget) {
        lv_obj_align(zmk_widget_wpm_status_obj(wpm_widget), LV_ALIGN_TOP_LEFT, 10, 50);
        // Restore cached values if available
        if (cached_status_valid) {
            zmk_widget_wpm_status_update(wpm_widget, &cached_keyboard_status);
        }
    }
    LOG_INF("✅ WPM status widget created");

#if !IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
    // Non-touch version: Signal status widget for display-only mode
    LOG_INF("Step 9: Init signal status widget (non-touch version)...");
    zmk_widget_signal_status_init(&signal_widget, screen);
    lv_obj_align(zmk_widget_signal_status_obj(&signal_widget), LV_ALIGN_BOTTOM_MID, 0, -5);
    LOG_INF("✅ Signal status widget initialized");
#else
    // Touch version: Signal widget disabled (swipe navigation instead)
    LOG_INF("Step 9: Signal status widget disabled (touch version)");
#endif

    // LOG_INF("Step 10: Init debug status widget...");
    // zmk_widget_debug_status_init(&debug_widget, screen);
    // bool debug_enabled = IS_ENABLED(CONFIG_PROSPECTOR_DEBUG_WIDGET);
    // LOG_INF("Debug widget %s by CONFIG_PROSPECTOR_DEBUG_WIDGET", debug_enabled ? "ENABLED" : "DISABLED");
    // zmk_widget_debug_status_set_visible(&debug_widget, debug_enabled);
    // LOG_INF("✅ Debug status widget initialized");  // DISABLED

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    LOG_INF("Step 11: Update scanner battery widget...");
    update_scanner_battery_widget();
    LOG_INF("✅ Scanner battery widget updated");
#endif

    LOG_INF("Step 12: Starting periodic signal monitoring...");
    start_signal_monitoring();
    LOG_INF("✅ Periodic monitoring started");

    LOG_INF("Step 13: System settings widget (dynamic allocation - created on demand)");
    // Widget will be created dynamically when needed (on swipe gesture)
    // zmk_widget_system_settings_init(&system_settings_widget, screen);  // REMOVED - now dynamic
    LOG_INF("✅ System settings widget setup complete");

    LOG_INF("Step 14: Keyboard list widget (dynamic allocation - created on demand)");
    // Widget will be created dynamically when needed (on swipe gesture)
    // zmk_widget_keyboard_list_init(&keyboard_list_widget, screen);  // REMOVED - now dynamic
    LOG_INF("✅ Keyboard list widget setup complete");

    LOG_INF("Step 15: Triggering scanner start...");
    trigger_scanner_start();
    LOG_INF("✅ Scanner start triggered");

    // Clear processing flag - screen init complete, swipe now allowed
    swipe_in_progress = false;
    LOG_DBG("🔓 Screen init completed - swipe enabled");

    LOG_INF("🎉 Scanner screen created successfully with gesture support");
    return screen;
}

// Late initialization to start scanner after display is ready
// Phase 5: Simplified - LVGL operations via message queue
static void start_scanner_delayed(struct k_work *work) {
    if (!device_name_label) {
        LOG_WRN("Display not ready yet, retrying scanner start...");
        k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(1));
        return;
    }

    LOG_INF("Starting BLE scanner...");

    // Register callback first
    int ret = zmk_status_scanner_register_callback(update_display_from_scanner);
    if (ret < 0) {
        LOG_ERR("Failed to register scanner callback: %d", ret);
        return;
    }

    // Start scanning
    ret = zmk_status_scanner_start();
    if (ret < 0) {
        LOG_ERR("Failed to start scanner: %d", ret);
        return;
    }

    LOG_INF("BLE scanner started successfully");
    // Phase 5: Send message to update label (thread-safe)
    scanner_msg_send_display_refresh();
}

static K_WORK_DELAYABLE_DEFINE(scanner_start_work, start_scanner_delayed);

// Trigger scanner start automatically when screen is created
static void trigger_scanner_start(void) {
    LOG_INF("Scheduling delayed scanner start from display creation");
    k_work_schedule(&scanner_start_work, K_SECONDS(3)); // Wait 3 seconds for display
}

// Helper functions for widget memory management
static void free_main_screen_widgets(void) {
    LOG_INF("🗑️  Freeing main screen widgets to save RAM...");

    // Delete modifier widget
    if (zmk_widget_modifier_status_obj(&modifier_widget)) {
        lv_obj_del(zmk_widget_modifier_status_obj(&modifier_widget));
        LOG_INF("  ✅ Modifier widget deleted");
    }

    // Delete layer widget
    if (zmk_widget_layer_status_obj(&layer_widget)) {
        lv_obj_del(zmk_widget_layer_status_obj(&layer_widget));
        LOG_INF("  ✅ Layer widget deleted");
    }

    LOG_INF("🗑️  Main screen widgets freed");
}

// DEPRECATED: Now using dynamic allocation - widgets are created/destroyed on demand
// static void free_keyboard_list_widgets(void) {
//     LOG_INF("🗑️  Freeing keyboard list widgets to save RAM...");
//
//     // Delete keyboard list widget (obj and all children)
//     if (keyboard_list_widget->obj) {
//         lv_obj_del(keyboard_list_widget->obj);
//         keyboard_list_widget->obj = NULL;
//         keyboard_list_widget->title_label = NULL;
//         LOG_INF("  ✅ Keyboard list widget deleted");
//     }
//
//     LOG_INF("🗑️  Keyboard list widgets freed");
// }

static void restore_main_screen_widgets(void) {
    LOG_INF("🔄 Restoring main screen widgets...");

    if (!main_screen) {
        LOG_ERR("❌ Cannot restore widgets - main_screen is NULL");
        return;
    }

    // Modifier widget now uses dynamic allocation - no need to recreate
    LOG_INF("  Modifier widget (dynamic allocation - created on demand)");
    // zmk_widget_modifier_status_init(&modifier_widget, main_screen);  // REMOVED - now dynamic
    // lv_obj_align(zmk_widget_modifier_status_obj(&modifier_widget), LV_ALIGN_BOTTOM_MID, 0, -65);
    LOG_INF("  ✅ Modifier widget ready");

    // Recreate layer widget
    LOG_INF("  Recreating layer widget...");
    zmk_widget_layer_status_init(&layer_widget, main_screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_widget), LV_ALIGN_CENTER, 0, 0);
    LOG_INF("  ✅ Layer widget recreated");

    LOG_INF("🔄 Main screen widgets restored");
}

static void restore_keyboard_list_widgets(void) {
    LOG_INF("🔄 Restoring keyboard list widgets...");

    if (!main_screen) {
        LOG_ERR("❌ Cannot restore widgets - main_screen is NULL");
        return;
    }

    // Keyboard list widget now uses dynamic allocation - no need to recreate
    LOG_INF("  Keyboard list widget (dynamic allocation - created on demand)");
    // zmk_widget_keyboard_list_init(&keyboard_list_widget, main_screen);  // REMOVED - now dynamic
    LOG_INF("  ✅ Keyboard list widget ready");

    LOG_INF("🔄 Keyboard list widgets restored");
}

#if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
// Swipe gesture event listener
// CRITICAL: ZMK event listeners run in WORK QUEUE context, NOT main thread!
// Therefore, we MUST NOT call LVGL APIs here - send message to main loop instead
static int swipe_gesture_listener(const zmk_event_t *eh) {
    const struct zmk_swipe_gesture_event *ev = as_zmk_swipe_gesture_event(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Restore brightness if dimmed due to timeout (touch wakes up display)
    // CRITICAL: Do NOT call brightness_control functions here - Work Queue context!
    // Send message to main loop instead for thread-safe processing
    if (timeout_dimmed) {
        #if IS_ENABLED(CONFIG_PROSPECTOR_TOUCH_ENABLED)
            scanner_msg_send_timeout_wake();
        #endif
    }

    // Map swipe direction to scanner_swipe_direction enum
    enum scanner_swipe_direction dir;
    switch (ev->direction) {
        case SWIPE_DIRECTION_UP:
            dir = SCANNER_SWIPE_UP;
            break;
        case SWIPE_DIRECTION_DOWN:
            dir = SCANNER_SWIPE_DOWN;
            break;
        case SWIPE_DIRECTION_LEFT:
            dir = SCANNER_SWIPE_LEFT;
            break;
        case SWIPE_DIRECTION_RIGHT:
            dir = SCANNER_SWIPE_RIGHT;
            break;
        default:
            return ZMK_EV_EVENT_BUBBLE;
    }

    // Phase 5: Send message to LVGL timer - NO LVGL calls here!
    // This runs in Work Queue context, LVGL operations will be done in main_loop_timer_cb
    LOG_DBG("🔄 Swipe event received in Work Queue, sending message: %d", dir);
    scanner_msg_send_swipe(dir);

    return ZMK_EV_EVENT_BUBBLE;
}

// Process swipe gesture - called from message queue handler
// This is the main swipe processing function for Phase 5 reconstruction
// Only needed in touch-enabled version
static void process_swipe_direction(int direction) {
    const char *dir_name[] = {"UP", "DOWN", "LEFT", "RIGHT"};
    LOG_INF("📥 Processing swipe from message queue: %s", dir_name[direction]);

    if (!main_screen) {
        LOG_ERR("❌ main_screen is NULL!");
        return;
    }

    // Processing guard: prevent concurrent swipe handling (deadlock prevention)
    if (swipe_in_progress) {
        LOG_WRN("⚠️  Swipe ignored - previous swipe still processing (deadlock prevention)");
        return;
    }

    // UI interaction guard: prevent swipe during slider/button interaction
    if (current_screen == SCREEN_DISPLAY_SETTINGS && display_settings_is_interacting()) {
        LOG_DBG("🎚️ Swipe ignored - UI interaction in progress");
        return;
    }

    // Cooldown check: prevent rapid repeated swipes
    uint32_t now = k_uptime_get_32();
    if (now - last_swipe_time < SWIPE_COOLDOWN_MS) {
        LOG_DBG("⏱️  Swipe ignored (cooldown: %d ms remaining)",
                (int)(SWIPE_COOLDOWN_MS - (now - last_swipe_time)));
        return;
    }
    last_swipe_time = now;

    // Set processing flag - prevents concurrent widget operations
    swipe_in_progress = true;

    // CRITICAL: Pause main loop timer to prevent widget access during destruction
    // Without this, timer may try to process messages while we're deleting widgets
    if (main_loop_timer) {
        lv_timer_pause(main_loop_timer);
        LOG_DBG("⏸️  Main loop timer paused for safe widget operations");
    }

    // Phase 5: ZMK event listener runs in main thread - no mutex needed
    // LVGL operations are safe here
    LOG_DBG("🔒 Swipe processing started");

    // Thread-safe LVGL operations (running in work queue context)
    // Handle gestures based on current screen state
    switch (direction) {
        case SWIPE_DIRECTION_DOWN:
            // Down swipe: Show Display Settings from main (has sliders - avoid left/right conflicts)
            if (current_screen == SCREEN_MAIN) {
                LOG_INF("⬇️  DOWN swipe from MAIN: Creating display settings widget");

                // Stop all periodic work queues
                stop_all_periodic_work();

                // Destroy main screen widgets to free memory
                // Destroy in reverse order of creation to minimize fragmentation
                if (device_name_label) {
                    lv_obj_del(device_name_label);
                    device_name_label = NULL;
                }
                if (modifier_widget) {
                    zmk_widget_modifier_status_destroy(modifier_widget);
                    modifier_widget = NULL;
                }
                if (layer_widget) {
                    zmk_widget_layer_status_destroy(layer_widget);
                    layer_widget = NULL;
                }
                if (battery_widget) {
                    zmk_widget_scanner_battery_destroy(battery_widget);
                    battery_widget = NULL;
                }
                if (wpm_widget) {
                    zmk_widget_wpm_status_destroy(wpm_widget);
                    wpm_widget = NULL;
                }
                if (connection_widget) {
                    zmk_widget_connection_status_destroy(connection_widget);
                    connection_widget = NULL;
                }
                LOG_DBG("✅ Main widgets destroyed for display settings");

                // Create display settings widget
                if (!display_settings_widget) {
                    display_settings_widget = zmk_widget_display_settings_create(main_screen);
                    if (!display_settings_widget) {
                        LOG_ERR("❌ Failed to create display settings widget");
                        break;
                    }

                    // Register LVGL input device for sliders/toggles
                    touch_handler_register_lvgl_indev();
                }

                zmk_widget_display_settings_show(display_settings_widget);
                current_screen = SCREEN_DISPLAY_SETTINGS;
            } else {
                // From any other screen: return to main
                LOG_INF("⬇️  DOWN swipe from other screen: Return to main");
                goto return_to_main;
            }
            break;

        case SWIPE_DIRECTION_UP:
            if (current_screen == SCREEN_MAIN) {
                // From main screen: create and show keyboard list (dynamic allocation)
                LOG_INF("⬆️  UP swipe from MAIN: Creating keyboard list widget");

                // Stop all periodic work queues to prevent interference
                stop_all_periodic_work();

                // Destroy main screen widgets to free memory for overlay
                // Destroy in reverse order of creation
                if (device_name_label) {
                    lv_obj_del(device_name_label);
                    device_name_label = NULL;
                }
                if (modifier_widget) {
                    zmk_widget_modifier_status_destroy(modifier_widget);
                    modifier_widget = NULL;
                }
                if (layer_widget) {
                    zmk_widget_layer_status_destroy(layer_widget);
                    layer_widget = NULL;
                }
                if (battery_widget) {
                    zmk_widget_scanner_battery_destroy(battery_widget);
                    battery_widget = NULL;
                }
                if (wpm_widget) {
                    zmk_widget_wpm_status_destroy(wpm_widget);
                    wpm_widget = NULL;
                }
                if (connection_widget) {
                    zmk_widget_connection_status_destroy(connection_widget);
                    connection_widget = NULL;
                }
                LOG_DBG("✅ Main widgets destroyed for keyboard list");

                // Create widget if not already created
                if (!keyboard_list_widget) {
                    keyboard_list_widget = zmk_widget_keyboard_list_create(main_screen);
                    if (!keyboard_list_widget) {
                        LOG_ERR("❌ Failed to create keyboard list widget");
                        break;  // Abort if creation failed
                    }

                    // Register LVGL input device for touch selection (first time only)
                    int ret = touch_handler_register_lvgl_indev();
                    if (ret < 0) {
                        LOG_ERR("❌ Failed to register LVGL input device: %d", ret);
                    } else {
                        LOG_INF("✅ LVGL input device registered for keyboard selection");
                    }
                }

                // Show the widget
                zmk_widget_keyboard_list_show(keyboard_list_widget);
                current_screen = SCREEN_KEYBOARD_LIST;
            } else if (current_screen == SCREEN_SETTINGS) {
                // From settings: return to main
                LOG_INF("⬆️  UP swipe from SETTINGS: Return to main");
                goto return_to_main;
            } else if (current_screen == SCREEN_KEYBOARD_LIST) {
                // Already on keyboard list screen, do nothing
                LOG_INF("⬆️  UP swipe: Already on keyboard list screen");
            } else {
                // From DISPLAY_SETTINGS: return to main
                LOG_INF("⬆️  UP swipe from other screen: Return to main");
                goto return_to_main;
            }
            break;

        case SWIPE_DIRECTION_LEFT:
            // Left swipe: Show System Settings (quick actions) from main
            if (current_screen == SCREEN_MAIN) {
                LOG_INF("⬅️  LEFT swipe from MAIN: Creating system settings widget");

                // Stop all periodic work queues to prevent interference
                stop_all_periodic_work();

                // Destroy main screen widgets to free memory for overlay
                // Destroy in reverse order of creation
                if (device_name_label) {
                    lv_obj_del(device_name_label);
                    device_name_label = NULL;
                }
                if (modifier_widget) {
                    zmk_widget_modifier_status_destroy(modifier_widget);
                    modifier_widget = NULL;
                }
                if (layer_widget) {
                    zmk_widget_layer_status_destroy(layer_widget);
                    layer_widget = NULL;
                }
                if (battery_widget) {
                    zmk_widget_scanner_battery_destroy(battery_widget);
                    battery_widget = NULL;
                }
                if (wpm_widget) {
                    zmk_widget_wpm_status_destroy(wpm_widget);
                    wpm_widget = NULL;
                }
                if (connection_widget) {
                    zmk_widget_connection_status_destroy(connection_widget);
                    connection_widget = NULL;
                }
                LOG_DBG("✅ Main widgets destroyed for settings");

                // Create widget if not already created
                if (!system_settings_widget) {
                    system_settings_widget = zmk_widget_system_settings_create(main_screen);
                    if (!system_settings_widget) {
                        LOG_ERR("❌ Failed to create system settings widget");
                        break;  // Abort if creation failed
                    }

                    // Register LVGL input device for button clicks (first time only)
                    int ret = touch_handler_register_lvgl_indev();
                    if (ret < 0) {
                        LOG_ERR("❌ Failed to register LVGL input device: %d", ret);
                    } else {
                        LOG_INF("✅ LVGL input device registered for button clicks");
                    }
                }

                // Show the widget
                zmk_widget_system_settings_show(system_settings_widget);
                current_screen = SCREEN_SETTINGS;
            } else {
                // From other screens, return to main
                LOG_INF("⬅️  LEFT swipe: Return to main screen");
                goto return_to_main;
            }
            break;

        case SWIPE_DIRECTION_RIGHT:
            // Right swipe always returns to main screen from any screen
            LOG_INF("➡️  RIGHT swipe: Return to main screen");
return_to_main:
            if (current_screen == SCREEN_SETTINGS) {
                if (system_settings_widget) {
                    zmk_widget_system_settings_hide(system_settings_widget);
                    zmk_widget_system_settings_destroy(system_settings_widget);
                    system_settings_widget = NULL;
                    LOG_INF("✅ System settings widget destroyed");
                }
            } else if (current_screen == SCREEN_DISPLAY_SETTINGS) {
                if (display_settings_widget) {
                    zmk_widget_display_settings_hide(display_settings_widget);
                    zmk_widget_display_settings_destroy(display_settings_widget);
                    display_settings_widget = NULL;
                    LOG_INF("✅ Display settings widget destroyed");
                }
            } else if (current_screen == SCREEN_KEYBOARD_LIST) {
                if (keyboard_list_widget) {
                    zmk_widget_keyboard_list_hide(keyboard_list_widget);
                    zmk_widget_keyboard_list_destroy(keyboard_list_widget);
                    keyboard_list_widget = NULL;
                    LOG_INF("✅ Keyboard list widget destroyed");
                }
            }

            current_screen = SCREEN_MAIN;

            // Apply scanner battery widget visibility from settings
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
            zmk_widget_scanner_battery_status_set_visible(&scanner_battery_widget,
                                                          display_settings_get_battery_visible_global());
            LOG_DBG("🔋 Scanner battery widget visibility: %s",
                    display_settings_get_battery_visible_global() ? "visible" : "hidden");
#endif

            // Recreate main screen widgets with cached values
            if (!device_name_label) {
                device_name_label = lv_label_create(main_screen);
                lv_obj_set_style_text_color(device_name_label, lv_color_white(), 0);
                lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_16, 0);
                lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 25);
                lv_label_set_text(device_name_label, cached_device_name);  // Restore cached name
                LOG_DBG("✅ Device name label recreated for main screen");
            }
            if (!wpm_widget) {
                wpm_widget = zmk_widget_wpm_status_create(main_screen);
                if (wpm_widget) {
                    lv_obj_align(zmk_widget_wpm_status_obj(wpm_widget), LV_ALIGN_TOP_LEFT, 10, 50);
                    // Restore cached values
                    if (cached_status_valid) {
                        zmk_widget_wpm_status_update(wpm_widget, &cached_keyboard_status);
                    }
                    LOG_DBG("✅ WPM widget recreated for main screen");
                }
            }
            // Keyboard battery widget (at bottom) - always recreate
            if (!battery_widget) {
                battery_widget = zmk_widget_scanner_battery_create(main_screen);
                if (battery_widget) {
                    lv_obj_align(zmk_widget_scanner_battery_obj(battery_widget), LV_ALIGN_BOTTOM_MID, 0, -20);
                    lv_obj_set_height(zmk_widget_scanner_battery_obj(battery_widget), 50);
                    // Restore cached values
                    if (cached_status_valid) {
                        zmk_widget_scanner_battery_update(battery_widget, &cached_keyboard_status);
                    }
                    LOG_DBG("✅ Keyboard battery widget recreated for main screen");
                }
            }
            if (!connection_widget) {
                connection_widget = zmk_widget_connection_status_create(main_screen);
                if (connection_widget) {
                    lv_obj_align(zmk_widget_connection_status_obj(connection_widget), LV_ALIGN_TOP_RIGHT, -5, 45);
                    // Restore cached values
                    if (cached_status_valid) {
                        zmk_widget_connection_status_update(connection_widget, &cached_keyboard_status);
                    }
                    LOG_DBG("✅ Connection widget recreated for main screen");
                }
            }
            if (!layer_widget) {
                layer_widget = zmk_widget_layer_status_create(main_screen);
                if (layer_widget) {
                    lv_obj_align(zmk_widget_layer_status_obj(layer_widget), LV_ALIGN_CENTER, 0, -10);
                    // Restore cached values
                    if (cached_status_valid) {
                        zmk_widget_layer_status_update(layer_widget, &cached_keyboard_status);
                    }
                    LOG_DBG("✅ Layer widget recreated for main screen");
                }
            }
            if (!modifier_widget) {
                modifier_widget = zmk_widget_modifier_status_create(main_screen);
                if (modifier_widget) {
                    lv_obj_align(zmk_widget_modifier_status_obj(modifier_widget), LV_ALIGN_CENTER, 0, 30);
                    // Restore cached values
                    if (cached_status_valid) {
                        zmk_widget_modifier_status_update(modifier_widget, &cached_keyboard_status);
                    }
                    LOG_DBG("✅ Modifier widget recreated for main screen");
                }
            }

            // Resume all periodic work queues
            resume_all_periodic_work();
            break;
    }

    // Resume main loop timer
    if (main_loop_timer) {
        lv_timer_resume(main_loop_timer);
        LOG_DBG("▶️  Main loop timer resumed after widget operations");
    }

    // Phase 5: Clear processing flag (no mutex to release)
    swipe_in_progress = false;
    LOG_DBG("🔓 Swipe processing completed");
}

// Phase 5: ZMK event listener no longer used for swipe processing
// Swipe is now processed entirely via message queue (touch_handler.c -> scanner_msg_send_swipe)
// This ensures all LVGL operations happen in main_loop_timer_cb (LVGL main thread)
// ZMK_LISTENER(swipe_gesture, swipe_gesture_listener);
// ZMK_SUBSCRIPTION(swipe_gesture, zmk_swipe_gesture_event);
#endif // CONFIG_PROSPECTOR_TOUCH_ENABLED

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY