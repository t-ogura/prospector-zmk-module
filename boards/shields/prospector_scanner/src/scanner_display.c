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
#include "scanner_battery_widget.h"
#include "connection_status_widget.h"
#include "layer_status_widget.h"
#include "modifier_status_widget.h"
// Profile widget removed - connection status already handled by connection_status_widget
#include "signal_status_widget.h"
#include "wpm_status_widget.h"
#include "debug_status_widget.h"
#include "scanner_battery_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Backward compatibility: provide defaults for v1.1.0 configs if not defined
#ifndef CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#define CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS 10  // 10% default
#endif

#ifndef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS
#define CONFIG_PROSPECTOR_FIXED_BRIGHTNESS 60   // 60% default
#endif

// External brightness control functions
void prospector_set_brightness(uint8_t brightness_percent);
void prospector_resume_brightness(void);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

// Global UI objects for dynamic updates
static lv_obj_t *device_name_label = NULL;
static struct zmk_widget_scanner_battery battery_widget;
static struct zmk_widget_connection_status connection_widget;
static struct zmk_widget_layer_status layer_widget;
static struct zmk_widget_modifier_status modifier_widget;
// Profile widget removed - redundant with connection status widget
static struct zmk_widget_signal_status signal_widget;
static struct zmk_widget_wpm_status wpm_widget;

// Global debug widget for sensor diagnostics (positioned in modifier area)
struct zmk_widget_debug_status debug_widget;

// Scanner's own battery status widget (top-right corner)
static struct zmk_widget_scanner_battery_status scanner_battery_widget;

#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
// Battery monitoring state - global to manage start/stop properly
static bool battery_monitoring_active = false;

// Backward compatibility: provide default interval if not defined
#ifndef CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S
#define CONFIG_PROSPECTOR_BATTERY_UPDATE_INTERVAL_S 120  // 2 minutes default
#endif
#endif

// Forward declaration
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

// Periodic signal timeout check (every 5 seconds)
static void check_signal_timeout_handler(struct k_work *work) {
    // Check signal widget for timeout
    zmk_widget_signal_status_check_timeout(&signal_widget);
    
    // Schedule next check
    k_work_schedule(&signal_timeout_work, K_SECONDS(5));
}

// 1Hz periodic RX update - called every second for smooth rate decline
static void periodic_rx_update_handler(struct k_work *work) {
    // Call periodic update for signal widget - ensures 1Hz update even without receptions
    zmk_widget_signal_status_periodic_update(&signal_widget);
    
    // Schedule next update in 1 second - this ensures continuous 1Hz updates
    k_work_schedule(&rx_periodic_work, K_SECONDS(1));
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
    LOG_INF("Started periodic signal timeout monitoring (5s intervals), 1Hz RX updates, and 5s battery debug updates");
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
    if (debug_widget.debug_label) {
        zmk_widget_debug_status_set_text(&debug_widget, debug_text);
    }

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
    if (debug_widget.debug_label) {
        zmk_widget_debug_status_set_text(&debug_widget, periodic_debug);
    }
    
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
    if (debug_widget.debug_label) {
        zmk_widget_debug_status_set_text(&debug_widget, "BATTERY MONITORING STARTED");
    }
}

// Stop battery monitoring when keyboards become inactive
static void stop_battery_monitoring(void) {
    k_work_cancel_delayable(&battery_periodic_work);
    LOG_INF("Stopped periodic battery monitoring - INACTIVE MODE");
    
    // Update debug widget to show monitoring stopped
    if (debug_widget.debug_label) {
        zmk_widget_debug_status_set_text(&debug_widget, "BATTERY MONITORING STOPPED");
    }
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
            prospector_set_brightness(CONFIG_PROSPECTOR_ADV_FREQUENCY_DIM_BRIGHTNESS);
            frequency_dimmed = true;
        }
    } else {
        // Frequency increased (keyboard became active), restore brightness
        if (frequency_dimmed) {
            LOG_INF("Advertisement frequency restored (%dms interval), resuming normal brightness", interval);
            prospector_resume_brightness();
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
        // No keyboards found - reset all widgets to default state
        lv_label_set_text(device_name_label, "Scanning...");
        
        // Reset all widgets to clear stale data
        zmk_widget_scanner_battery_reset(&battery_widget);
        zmk_widget_connection_status_reset(&connection_widget);
        zmk_widget_layer_status_reset(&layer_widget);
        zmk_widget_modifier_status_reset(&modifier_widget);
        zmk_widget_signal_status_reset(&signal_widget);
        zmk_widget_wpm_status_reset(&wpm_widget);
        
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
        prospector_set_brightness(CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS);
#else
        // When ALS is disabled, reduce to 20% of configured brightness
        prospector_set_brightness(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS / 5);
#endif
        
        LOG_INF("Display updated: No keyboards - all widgets reset, brightness reduced");
    } else {
        // Find active keyboards and display their info  
        for (int i = 0; i < ZMK_STATUS_SCANNER_MAX_KEYBOARDS; i++) {
            struct zmk_keyboard_status *kbd = zmk_status_scanner_get_keyboard(i);
            if (kbd && kbd->active) {
                // Update device name (large, prominent display)
                lv_label_set_text(device_name_label, kbd->ble_name);
                
                // Update all widgets
                zmk_widget_scanner_battery_update(&battery_widget, kbd);
                zmk_widget_connection_status_update(&connection_widget, kbd);
                zmk_widget_layer_status_update(&layer_widget, kbd);
                zmk_widget_modifier_status_update(&modifier_widget, kbd);
                
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
                    zmk_widget_signal_status_update(&signal_widget, kbd->rssi);
                    
                    // Remember last values for change detection
                    last_layer = kbd->data.active_layer;
                    last_wpm = kbd->data.wpm_value;
                    last_battery = kbd->data.battery_level;
                    last_modifier = kbd->data.modifier_flags;
                }
                
                zmk_widget_wpm_status_update(&wpm_widget, kbd);
                
                // Resume normal brightness control when keyboard is connected
                prospector_resume_brightness();
                
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
    LOG_INF("Initializing scanner display system");
    
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display)) {
        LOG_ERR("Display device not ready");
        return -EIO;
    }
    
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
    
    LOG_INF("Scanner display initialized successfully");
    return 0;
}

// Initialize display early in the boot process
SYS_INIT(scanner_display_init, APPLICATION, 60);

// Required function for ZMK_DISPLAY_STATUS_SCREEN_CUSTOM
// Following the working adapter pattern with simple, stable display
lv_obj_t *zmk_display_status_screen() {
    LOG_INF("Creating scanner status screen");
    
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);
    
    // Device name label at top center (larger font for better readability) - moved down 10px
    device_name_label = lv_label_create(screen);
    lv_obj_set_style_text_color(device_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(device_name_label, &lv_font_unscii_16, 0); // Retro pixel font style
    lv_obj_align(device_name_label, LV_ALIGN_TOP_MID, 0, 25); // Back to center
    lv_label_set_text(device_name_label, "Initializing...");
    
    // Scanner battery status widget in top right corner (above connection status)
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    zmk_widget_scanner_battery_status_init(&scanner_battery_widget, screen);
    lv_obj_align(zmk_widget_scanner_battery_status_obj(&scanner_battery_widget), LV_ALIGN_TOP_RIGHT, 10, 0);
#endif
    
    // Connection status widget in top right - moved down to make room for battery
    zmk_widget_connection_status_init(&connection_widget, screen);
    lv_obj_align(zmk_widget_connection_status_obj(&connection_widget), LV_ALIGN_TOP_RIGHT, -5, 45); // Original position
    
    // Layer status widget in the center (horizontal layer display) - moved down 10px
    zmk_widget_layer_status_init(&layer_widget, screen);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_widget), LV_ALIGN_CENTER, 0, -10); // Back to center
    
    // Modifier status widget between layer and battery - moved down 10px
    zmk_widget_modifier_status_init(&modifier_widget, screen);
    lv_obj_align(zmk_widget_modifier_status_obj(&modifier_widget), LV_ALIGN_CENTER, 0, 30); // Back to center
    
    // Profile widget removed - BLE profile already shown in connection status widget
    
    // Battery widget moved down 20px more as requested (was -40, now -20)
    zmk_widget_scanner_battery_init(&battery_widget, screen);
    lv_obj_align(zmk_widget_scanner_battery_obj(&battery_widget), LV_ALIGN_BOTTOM_MID, 0, -20); // Back to center
    lv_obj_set_height(zmk_widget_scanner_battery_obj(&battery_widget), 50);
    
    // WPM status widget - positioned above layer display, left-aligned
    zmk_widget_wpm_status_init(&wpm_widget, screen);
    lv_obj_align(zmk_widget_wpm_status_obj(&wpm_widget), LV_ALIGN_TOP_LEFT, 10, 50); // Top-left corner positioning
    
    // Signal status widget (RSSI + reception rate) at the very bottom with full width space
    zmk_widget_signal_status_init(&signal_widget, screen);
    lv_obj_align(zmk_widget_signal_status_obj(&signal_widget), LV_ALIGN_BOTTOM_RIGHT, -5, -5); // More space from edge
    
    // Debug status widget (overlaps modifier area when no modifiers active)
    zmk_widget_debug_status_init(&debug_widget, screen);
    
    // Debug widget visibility controlled by CONFIG_PROSPECTOR_DEBUG_WIDGET
    bool debug_enabled = IS_ENABLED(CONFIG_PROSPECTOR_DEBUG_WIDGET);
    LOG_INF("Debug widget %s by CONFIG_PROSPECTOR_DEBUG_WIDGET", debug_enabled ? "ENABLED" : "DISABLED");
    zmk_widget_debug_status_set_visible(&debug_widget, debug_enabled);
    if (debug_widget.debug_label && debug_enabled) {
        zmk_widget_debug_status_set_text(&debug_widget, "DEBUG: Scanner Ready");
        LOG_INF("✅ Debug widget initialized for diagnostics");
    }
    
    // Initialize scanner battery widget with current status
#if IS_ENABLED(CONFIG_PROSPECTOR_BATTERY_SUPPORT)
    LOG_INF("Initializing scanner battery widget with current status");
    update_scanner_battery_widget();
    
    // NOTE: Battery monitoring will start automatically when keyboards become active
    // This saves power when no keyboards are connected
#endif
    
    // Start periodic signal timeout monitoring
    start_signal_monitoring();
    
    // Trigger scanner initialization after screen is ready
    trigger_scanner_start();
    
    LOG_INF("Scanner screen created successfully");
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

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY