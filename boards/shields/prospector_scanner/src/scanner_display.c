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
// Profile widget removed - connection status already handled by connection_status_widget
// #include "signal_status_widget.h"  // DISABLED - info available in keyboard list
#include "wpm_status_widget.h"  // RE-ENABLED
// #include "debug_status_widget.h"  // DISABLED - debug only
#include "scanner_battery_status_widget.h"
#include "system_settings_widget.h"
#include "keyboard_list_widget.h"  // Keyboard list screen (shows active keyboards)
#include "touch_handler.h"
#include "events/swipe_gesture_event.h"

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
// Profile widget removed - redundant with connection status widget
// static struct zmk_widget_signal_status signal_widget;  // DISABLED - info in keyboard list
// WPM widget (DYNAMIC ALLOCATION - created when main screen shown, destroyed when switching to overlays)
static struct zmk_widget_wpm_status *wpm_widget = NULL;
// System settings widget for settings screen (DYNAMIC ALLOCATION)
static struct zmk_widget_system_settings *system_settings_widget = NULL;
// Keyboard list widget for showing active keyboards (DYNAMIC ALLOCATION)
static struct zmk_widget_keyboard_list *keyboard_list_widget = NULL;

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

// Screen state management
enum screen_state {
    SCREEN_MAIN,           // Main status screen
    SCREEN_SETTINGS,       // System settings screen
    SCREEN_KEYBOARD_LIST   // Active keyboards list screen
};

static enum screen_state current_screen = SCREEN_MAIN;

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

// Periodic signal timeout check (every 5 seconds)
static void check_signal_timeout_handler(struct k_work *work) {
    // Check signal widget for timeout - DISABLED
    // zmk_widget_signal_status_check_timeout(&signal_widget);

    // Schedule next check
    k_work_schedule(&signal_timeout_work, K_SECONDS(5));
}

// 1Hz periodic RX update - called every second for smooth rate decline
static void periodic_rx_update_handler(struct k_work *work) {
    // Call periodic update for signal widget - DISABLED
    // zmk_widget_signal_status_periodic_update(&signal_widget);

    // Schedule next update in 1 second - this ensures continuous 1Hz updates
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

// Battery debug update handler - constant display for troubleshooting
static void battery_debug_update_handler(struct k_work *work) {
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    // Force battery widget update for constant debug visibility
    update_scanner_battery_widget();
#else
    // Battery support disabled - skip battery widget update
    LOG_DBG("Battery debug update skipped - battery support disabled");
#endif

    // Schedule next update in 5 seconds
    k_work_schedule(&battery_debug_work, K_SECONDS(5));
}

// Start periodic signal monitoring
static void start_signal_monitoring(void) {
    k_work_schedule(&signal_timeout_work, K_SECONDS(5));
    k_work_schedule(&rx_periodic_work, K_SECONDS(1));  // Start 1Hz updates
    k_work_schedule(&battery_debug_work, K_SECONDS(2)); // Start battery debug updates (2s delay)
    k_work_schedule(&memory_monitor_work, K_SECONDS(10)); // Uptime monitor (10s interval)
    LOG_INF("Started periodic monitoring: signal timeout (5s), RX updates (1Hz), battery debug (5s), uptime (10s)");
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
static int scanner_battery_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev) {
        LOG_INF("🔋 Scanner battery event: %d%% (state changed)", ev->state_of_charge);
        update_scanner_battery_widget();
        return 0;
    }
    
    return -ENOTSUP;
}

// USB connection state changed event handler  
static int scanner_usb_listener(const zmk_event_t *eh) {
    if (as_zmk_usb_conn_state_changed(eh)) {
        LOG_DBG("Scanner USB connection state changed event received");
        update_scanner_battery_widget();
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

// Periodic battery status update work
static void battery_periodic_update_handler(struct k_work *work) {
    static uint32_t periodic_counter = 0;
    periodic_counter++;
    
    LOG_INF("🔄 Periodic battery status update triggered (%ds interval)", CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S);
    
    // CRITICAL INVESTIGATION: Bypass ZMK battery cache - read from hardware directly
    uint8_t current_battery = 0;
    bool current_usb = false;
    bool current_charging = false;
    
    // METHOD 1: ZMK API (may be cached)
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    uint8_t zmk_cached_battery = zmk_battery_state_of_charge();
    LOG_INF("🔍 ZMK API CALL: zmk_battery_state_of_charge() returned %d%% (possibly cached)", zmk_cached_battery);
    current_battery = zmk_cached_battery;
#endif

    // METHOD 2: Direct hardware sensor access
#if DT_HAS_CHOSEN(zmk_battery)
    const struct device *battery_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
    if (device_is_ready(battery_dev)) {
        struct sensor_value voltage;
        int ret = sensor_sample_fetch(battery_dev);
        if (ret == 0) {
            ret = sensor_channel_get(battery_dev, SENSOR_CHAN_VOLTAGE, &voltage);
            if (ret == 0) {
                // Convert voltage to percentage (rough estimation)
                // Typical Li-Po: 3.0V (0%) to 4.2V (100%)
                int32_t voltage_mv = voltage.val1 * 1000 + voltage.val2 / 1000;
                
                // Simple voltage-to-percentage mapping
                uint8_t hardware_battery = 0;
                if (voltage_mv >= 4200) {
                    hardware_battery = 100;
                } else if (voltage_mv >= 4000) {
                    hardware_battery = 75 + ((voltage_mv - 4000) * 25) / 200;
                } else if (voltage_mv >= 3700) {
                    hardware_battery = 25 + ((voltage_mv - 3700) * 50) / 300;
                } else if (voltage_mv >= 3000) {
                    hardware_battery = ((voltage_mv - 3000) * 25) / 700;
                } else {
                    hardware_battery = 0;
                }
                
                LOG_INF("🔍 HARDWARE SENSOR: Battery voltage %dmV -> %d%% (hardware calc)", voltage_mv, hardware_battery);
                
                // Compare ZMK cache vs hardware reading
                if (zmk_cached_battery != hardware_battery) {
                    LOG_WRN("⚠️  CACHE MISMATCH: ZMK cached=%d%% vs Hardware=%d%% (diff=%d%%)", 
                            zmk_cached_battery, hardware_battery, abs(zmk_cached_battery - hardware_battery));
                    // Use hardware reading if significantly different
                    if (abs(zmk_cached_battery - hardware_battery) > 5) {
                        current_battery = hardware_battery;
                        LOG_INF("🎯 USING HARDWARE VALUE due to significant difference");
                    }
                } else {
                    LOG_INF("✅ ZMK cache matches hardware reading");
                }
            } else {
                LOG_WRN("Failed to read battery voltage from sensor: %d", ret);
            }
        } else {
            LOG_WRN("Failed to sample battery sensor: %d", ret);
        }
    } else {
        LOG_WRN("Battery sensor device not ready");
    }
#endif

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    current_usb = zmk_usb_is_powered();
    current_charging = current_usb && (current_battery < 100);
    LOG_INF("🔍 USB API CALL: zmk_usb_is_powered() returned %s", current_usb ? "true" : "false");
#endif

    LOG_INF("🔍 FINAL VALUES: Battery=%d%% USB=%s Charging=%s", 
            current_battery, current_usb ? "true" : "false", current_charging ? "true" : "false");
    
    // Update debug widget with comprehensive battery investigation (line 1)
    static char periodic_debug[128];
    
    // Show both ZMK cached and hardware values for comparison
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    uint8_t zmk_only = zmk_battery_state_of_charge();
#else
    uint8_t zmk_only = 0;
#endif
    
    snprintf(periodic_debug, sizeof(periodic_debug), "P#%d ZMK:%d%% HW:%d%% U:%s\nMethod:%s Chg:%s", 
             periodic_counter, zmk_only, current_battery, current_usb ? "Y" : "N",
             (current_battery != zmk_only) ? "HW" : "CACHE",
             current_charging ? "Y" : "N");
    // TEMPORARILY DISABLED: debug widget updates to avoid overwriting brightness_control messages
    // if (debug_widget.debug_label) {
    //     zmk_widget_debug_status_set_text(&debug_widget, periodic_debug);
    // }
    
    // FORCE UPDATE regardless of cache - bypass the change detection in update_scanner_battery_widget()
    LOG_INF("🔍 BYPASSING cache check - forcing widget update directly");
    zmk_widget_scanner_battery_status_update(&scanner_battery_widget, 
                                            current_battery, current_usb, current_charging);
    
    // Also call the original update method for comparison
    update_scanner_battery_widget();
    
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
static void update_display_from_scanner(struct zmk_status_scanner_event_data *event_data) {
    if (!device_name_label) {
        return; // UI not ready yet
    }
    
    LOG_INF("Scanner event received: %d for keyboard %d", event_data->event, event_data->keyboard_index);
    
#if IS_ENABLED(CONFIG_PROSPECTOR_ADVERTISEMENT_FREQUENCY_DIM)
    // Monitor advertisement frequency for automatic brightness adjustment
    check_advertisement_frequency();
#endif
    
    int active_count = zmk_status_scanner_get_active_count();
    
    if (active_count == 0) {
        // No keyboards found - reset all widgets to default state (NULL-safe for dynamic allocation)
        if (device_name_label) {
            lv_label_set_text(device_name_label, "Scanning...");
        }
        
        // Reset all widgets to clear stale data (NULL-safe for dynamic allocation)
        if (battery_widget) {
            zmk_widget_scanner_battery_reset(battery_widget);
        }
        zmk_widget_connection_status_reset(&connection_widget);
        zmk_widget_layer_status_reset(&layer_widget);

        // Modifier widget - destroy if exists (dynamic allocation)
        if (modifier_widget) {
            zmk_widget_modifier_status_destroy(modifier_widget);
            modifier_widget = NULL;
        }

        // zmk_widget_signal_status_reset(&signal_widget);  // DISABLED
        // WPM widget reset (NULL-safe for dynamic allocation)
        if (wpm_widget) {
            zmk_widget_wpm_status_reset(wpm_widget);
        }
        
        // Reset scanner's own battery widget (don't reset - should show scanner status)
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT) 
        // Scanner battery widget shows scanner's own status, not keyboard status
        // Don't reset it when keyboards disconnect - scanner battery is independent
        
        // Stop battery monitoring when no keyboards are active
        stop_battery_monitoring();
        
        // Reset battery monitoring state so it will restart when keyboards return
        battery_monitoring_active = false;
#endif
        
        // Reset advertisement frequency monitoring when no keyboards
#if IS_ENABLED(CONFIG_PROSPECTOR_ADVERTISEMENT_FREQUENCY_DIM)
        last_advertisement_time = 0;
        frequency_dimmed = false;
#endif
        
        // Reduce brightness when no keyboards are connected
#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
        // When ALS is enabled, set to minimum brightness
        // prospector_set_brightness(CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS);
#else
        // When ALS is disabled, reduce to 20% of configured brightness
        // prospector_set_brightness(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS / 5);
#endif
        
        LOG_INF("Display updated: No keyboards - all widgets reset, brightness reduced");
    } else {
        // Find active keyboards and display their info  
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
            if (kbd && kbd->active) {
                // Cache keyboard status for widget restoration
                memcpy(&cached_keyboard_status, kbd, sizeof(struct zmk_keyboard_status));
                cached_status_valid = true;

                // Cache device name
                strncpy(cached_device_name, kbd->ble_name, sizeof(cached_device_name) - 1);
                cached_device_name[sizeof(cached_device_name) - 1] = '\0';

                // Update device name (large, prominent display - NULL-safe for dynamic allocation)
                if (device_name_label) {
                    lv_label_set_text(device_name_label, kbd->ble_name);
                }

                // Update all widgets (NULL-safe for dynamic allocation)
                if (battery_widget) {
                    zmk_widget_scanner_battery_update(battery_widget, kbd);
                }
                if (connection_widget) {
                    zmk_widget_connection_status_update(connection_widget, kbd);
                }
                if (layer_widget) {
                    zmk_widget_layer_status_update(layer_widget, kbd);
                }

                // Modifier widget - dynamic allocation based on active modifiers
                bool has_modifiers = (kbd->data.modifier_flags != 0);
                if (has_modifiers) {
                    // Create widget if modifiers are active and widget doesn't exist
                    if (!modifier_widget) {
                        modifier_widget = zmk_widget_modifier_status_create(main_screen);
                        if (modifier_widget) {
                            lv_obj_align(zmk_widget_modifier_status_obj(modifier_widget),
                                       LV_ALIGN_CENTER, 0, 30);
                        }
                    }
                    // Update widget if it exists
                    if (modifier_widget) {
                        zmk_widget_modifier_status_update(modifier_widget, kbd);
                    }
                } else {
                    // Destroy widget if no modifiers are active
                    if (modifier_widget) {
                        zmk_widget_modifier_status_destroy(modifier_widget);
                        modifier_widget = NULL;
                    }
                }
                
                // Only update signal/RX when we receive meaningful data updates
                // Check if this is a real data update by monitoring multiple fields
                // This prevents counting duplicate advertisements or scan responses
                static uint8_t last_layer = 255;
                static uint8_t last_wpm = 255;
                static uint8_t last_battery = 255;
                static uint8_t last_modifier = 255;
                
                bool data_changed = (kbd->data.active_layer != last_layer) ||
                                   (kbd->data.wpm_value != last_wpm) ||
                                   (kbd->data.battery_level != last_battery) ||
                                   (kbd->data.modifier_flags != last_modifier);
                
                if (data_changed) {
                    // Data actually changed - this is a real update from the keyboard
                    // zmk_widget_signal_status_update(&signal_widget, kbd->rssi);  // DISABLED
                    
                    // Remember last values for change detection
                    last_layer = kbd->data.active_layer;
                    last_wpm = kbd->data.wpm_value;
                    last_battery = kbd->data.battery_level;
                    last_modifier = kbd->data.modifier_flags;
                }

                // WPM widget update (NULL-safe for dynamic allocation)
                if (wpm_widget) {
                    zmk_widget_wpm_status_update(wpm_widget, kbd);
                }

                // Resume normal brightness control when keyboard is connected
                // prospector_resume_brightness(); // Function removed in v1.1.1
                
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
                // Start battery monitoring when keyboards become active
                if (!battery_monitoring_active) {
                    start_battery_monitoring();
                    battery_monitoring_active = true;
                }
#endif
                
                // Enhanced debug logging including modifier flags
                LOG_INF("🔧 SCANNER: Raw keyboard data - modifier_flags=0x%02X", kbd->data.modifier_flags);
                
                if (kbd->data.device_role == ZMK_DEVICE_ROLE_CENTRAL && 
                    kbd->data.peripheral_battery[0] > 0) {
                    LOG_INF("Split keyboard: %s, Central %d%%, Left %d%%, Layer: %d, Mods: 0x%02X", 
                            kbd->ble_name, kbd->data.battery_level, 
                            kbd->data.peripheral_battery[0], kbd->data.active_layer, kbd->data.modifier_flags);
                } else {
                    LOG_INF("Keyboard: %s, Battery %d%%, Layer: %d, Mods: 0x%02X", 
                            kbd->ble_name, kbd->data.battery_level, kbd->data.active_layer, kbd->data.modifier_flags);
                }
                break; // Only handle the first active keyboard for now
            }
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
    
    // Note: Backlight control is now handled by brightness_control.c

    // Add a delay to allow display to stabilize
    k_msleep(100);

    // Initialize direct touch handler for raw coordinate debugging
    ret = touch_handler_init();
    if (ret < 0) {
        LOG_WRN("Touch handler init failed: %d (continuing anyway)", ret);
    } else {
        LOG_INF("✅ Touch handler initialized - will log raw coordinates");
    }

    // Note: LVGL input device will be registered when Settings screen is first opened
    // (dynamic allocation - only register when buttons are actually created)

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

    LOG_INF("Step 1: Creating main screen object...");
    lv_obj_t *screen = lv_obj_create(NULL);
    main_screen = screen;  // Save reference for later use
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);
    LOG_INF("✅ Main screen created");

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
    LOG_INF("✅ Scanner battery status widget initialized");
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

    // LOG_INF("Step 9: Init signal status widget...");
    // zmk_widget_signal_status_init(&signal_widget, screen);
    // lv_obj_align(zmk_widget_signal_status_obj(&signal_widget), LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    // LOG_INF("✅ Signal status widget initialized");  // DISABLED

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

    LOG_INF("🎉 Scanner screen created successfully with gesture support");
    return screen;
}

// Late initialization to start scanner after display is ready
static void start_scanner_delayed(struct k_work *work) {
    if (!device_name_label) {
        LOG_WRN("Display not ready yet, retrying scanner start...");
        k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(1));
        return;
    }
    
    LOG_INF("Starting BLE scanner...");
    lv_label_set_text(device_name_label, "Starting scanner...");
    
    // Register callback first
    int ret = zmk_status_scanner_register_callback(update_display_from_scanner);
    if (ret < 0) {
        LOG_ERR("Failed to register scanner callback: %d", ret);
        lv_label_set_text(device_name_label, "Scanner Error");
        return;
    }
    
    // Start scanning
    ret = zmk_status_scanner_start();
    if (ret < 0) {
        LOG_ERR("Failed to start scanner: %d", ret);
        lv_label_set_text(device_name_label, "Start Error");
        return;
    }
    
    LOG_INF("BLE scanner started successfully");
    lv_label_set_text(device_name_label, "Scanning...");
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

// Swipe gesture cooldown to prevent rapid repeated swipes causing issues
// Increased to 500ms to account for dynamic memory allocation overhead
#define SWIPE_COOLDOWN_MS 500  // 500ms cooldown between swipes
static uint32_t last_swipe_time = 0;

// Swipe gesture event listener (runs in main thread - safe for LVGL)
static int swipe_gesture_listener(const zmk_event_t *eh) {
    const struct zmk_swipe_gesture_event *ev = as_zmk_swipe_gesture_event(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const char *dir_name[] = {"UP", "DOWN", "LEFT", "RIGHT"};
    LOG_INF("📥 Swipe event received in display thread: %s", dir_name[ev->direction]);

    if (!main_screen) {
        LOG_ERR("❌ main_screen is NULL!");
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Cooldown check: prevent rapid repeated swipes
    uint32_t now = k_uptime_get_32();
    if (now - last_swipe_time < SWIPE_COOLDOWN_MS) {
        LOG_DBG("⏱️  Swipe ignored (cooldown: %d ms remaining)",
                (int)(SWIPE_COOLDOWN_MS - (now - last_swipe_time)));
        return ZMK_EV_EVENT_BUBBLE;
    }
    last_swipe_time = now;

    // Thread-safe LVGL operations (running in main thread via event system)
    // Handle gestures based on current screen state
    switch (ev->direction) {
        case SWIPE_DIRECTION_DOWN:
            if (current_screen == SCREEN_MAIN) {
                // From main screen: create and show settings (dynamic allocation)
                LOG_INF("⬇️  DOWN swipe from MAIN: Creating system settings widget");

                // Destroy main screen widgets to free memory for overlay
                if (wpm_widget) {
                    zmk_widget_wpm_status_destroy(wpm_widget);
                    wpm_widget = NULL;
                    LOG_DBG("✅ WPM widget destroyed to free memory for overlay");
                }
                if (battery_widget) {
                    zmk_widget_scanner_battery_destroy(battery_widget);
                    battery_widget = NULL;
                    LOG_DBG("✅ Battery widget destroyed to free memory for overlay");
                }
                if (connection_widget) {
                    zmk_widget_connection_status_destroy(connection_widget);
                    connection_widget = NULL;
                    LOG_DBG("✅ Connection widget destroyed to free memory for overlay");
                }
                if (layer_widget) {
                    zmk_widget_layer_status_destroy(layer_widget);
                    layer_widget = NULL;
                    LOG_DBG("✅ Layer widget destroyed to free memory for overlay");
                }
                if (device_name_label) {
                    lv_obj_del(device_name_label);
                    device_name_label = NULL;
                    LOG_DBG("✅ Device name label destroyed to free memory for overlay");
                }

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
            } else if (current_screen == SCREEN_KEYBOARD_LIST) {
                // From keyboard list: return to main (hide and destroy widget)
                LOG_INF("⬇️  DOWN swipe from KEYBOARD_LIST: Return to main");
                if (keyboard_list_widget) {
                    zmk_widget_keyboard_list_hide(keyboard_list_widget);
                    zmk_widget_keyboard_list_destroy(keyboard_list_widget);
                    keyboard_list_widget = NULL;
                    LOG_INF("✅ Keyboard list widget destroyed, memory freed");
                }
                current_screen = SCREEN_MAIN;

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
                if (!battery_widget) {
                    battery_widget = zmk_widget_scanner_battery_create(main_screen);
                    if (battery_widget) {
                        lv_obj_align(zmk_widget_scanner_battery_obj(battery_widget), LV_ALIGN_BOTTOM_MID, 0, -20);
                        lv_obj_set_height(zmk_widget_scanner_battery_obj(battery_widget), 50);
                        // Restore cached values
                        if (cached_status_valid) {
                            zmk_widget_scanner_battery_update(battery_widget, &cached_keyboard_status);
                        }
                        LOG_DBG("✅ Battery widget recreated for main screen");
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
            } else if (current_screen == SCREEN_SETTINGS) {
                // Already on settings screen, do nothing
                LOG_INF("⬇️  DOWN swipe: Already on settings screen");
            }
            break;

        case SWIPE_DIRECTION_UP:
            if (current_screen == SCREEN_MAIN) {
                // From main screen: create and show keyboard list (dynamic allocation)
                LOG_INF("⬆️  UP swipe from MAIN: Creating keyboard list widget");

                // Destroy main screen widgets to free memory for overlay
                if (wpm_widget) {
                    zmk_widget_wpm_status_destroy(wpm_widget);
                    wpm_widget = NULL;
                    LOG_DBG("✅ WPM widget destroyed to free memory for overlay");
                }
                if (battery_widget) {
                    zmk_widget_scanner_battery_destroy(battery_widget);
                    battery_widget = NULL;
                    LOG_DBG("✅ Battery widget destroyed to free memory for overlay");
                }
                if (connection_widget) {
                    zmk_widget_connection_status_destroy(connection_widget);
                    connection_widget = NULL;
                    LOG_DBG("✅ Connection widget destroyed to free memory for overlay");
                }
                if (layer_widget) {
                    zmk_widget_layer_status_destroy(layer_widget);
                    layer_widget = NULL;
                    LOG_DBG("✅ Layer widget destroyed to free memory for overlay");
                }
                if (device_name_label) {
                    lv_obj_del(device_name_label);
                    device_name_label = NULL;
                    LOG_DBG("✅ Device name label destroyed to free memory for overlay");
                }

                // Create widget if not already created
                if (!keyboard_list_widget) {
                    keyboard_list_widget = zmk_widget_keyboard_list_create(main_screen);
                    if (!keyboard_list_widget) {
                        LOG_ERR("❌ Failed to create keyboard list widget");
                        break;  // Abort if creation failed
                    }
                }

                // Show the widget
                zmk_widget_keyboard_list_show(keyboard_list_widget);
                current_screen = SCREEN_KEYBOARD_LIST;
            } else if (current_screen == SCREEN_SETTINGS) {
                // From settings: return to main (hide and destroy widget)
                LOG_INF("⬆️  UP swipe from SETTINGS: Return to main");
                if (system_settings_widget) {
                    zmk_widget_system_settings_hide(system_settings_widget);
                    zmk_widget_system_settings_destroy(system_settings_widget);
                    system_settings_widget = NULL;
                    LOG_INF("✅ System settings widget destroyed, memory freed");
                }
                current_screen = SCREEN_MAIN;

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
                if (!battery_widget) {
                    battery_widget = zmk_widget_scanner_battery_create(main_screen);
                    if (battery_widget) {
                        lv_obj_align(zmk_widget_scanner_battery_obj(battery_widget), LV_ALIGN_BOTTOM_MID, 0, -20);
                        lv_obj_set_height(zmk_widget_scanner_battery_obj(battery_widget), 50);
                        // Restore cached values
                        if (cached_status_valid) {
                            zmk_widget_scanner_battery_update(battery_widget, &cached_keyboard_status);
                        }
                        LOG_DBG("✅ Battery widget recreated for main screen");
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
            } else if (current_screen == SCREEN_KEYBOARD_LIST) {
                // Already on keyboard list screen, do nothing
                LOG_INF("⬆️  UP swipe: Already on keyboard list screen");
            }
            break;

        case SWIPE_DIRECTION_LEFT:
        case SWIPE_DIRECTION_RIGHT:
            // Left/Right swipe always returns to main screen from any screen
            LOG_INF("⬅️➡️  LEFT/RIGHT swipe: Return to main screen");
            if (current_screen == SCREEN_SETTINGS) {
                // Hide and destroy settings widget
                if (system_settings_widget) {
                    zmk_widget_system_settings_hide(system_settings_widget);
                    zmk_widget_system_settings_destroy(system_settings_widget);
                    system_settings_widget = NULL;
                    LOG_INF("✅ System settings widget destroyed, memory freed");
                }
            } else if (current_screen == SCREEN_KEYBOARD_LIST) {
                // Hide and destroy widget (free memory)
                if (keyboard_list_widget) {
                    zmk_widget_keyboard_list_hide(keyboard_list_widget);
                    zmk_widget_keyboard_list_destroy(keyboard_list_widget);
                    keyboard_list_widget = NULL;
                    LOG_INF("✅ Keyboard list widget destroyed, memory freed");
                }
            }
            current_screen = SCREEN_MAIN;

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
            if (!battery_widget) {
                battery_widget = zmk_widget_scanner_battery_create(main_screen);
                if (battery_widget) {
                    lv_obj_align(zmk_widget_scanner_battery_obj(battery_widget), LV_ALIGN_BOTTOM_MID, 0, -20);
                    lv_obj_set_height(zmk_widget_scanner_battery_obj(battery_widget), 50);
                    // Restore cached values
                    if (cached_status_valid) {
                        zmk_widget_scanner_battery_update(battery_widget, &cached_keyboard_status);
                    }
                    LOG_DBG("✅ Battery widget recreated for main screen");
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
            break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(swipe_gesture, swipe_gesture_listener);
ZMK_SUBSCRIPTION(swipe_gesture, zmk_swipe_gesture_event);

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY