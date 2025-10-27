/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 * 
 * Safe Brightness Control for Prospector v1.1.1
 * - CONFIG=n: Fixed brightness mode only
 * - CONFIG=y: Sensor mode (requires APDS9960 hardware)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include "debug_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
    #warning "COMPILING SENSOR MODE - CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y"
#else
    #warning "COMPILING FIXED MODE - CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n"
#endif

// Only compile brightness control if sensor mode is DISABLED
#if !IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)

// Delayed debug message function for fixed mode
static void delayed_debug_msg(struct k_work *work) {
    zmk_widget_debug_status_set_text(&debug_widget, "🔆 FIXED MODE ACTIVE");
}

// Fixed brightness mode implementation - safe and simple
static int brightness_control_init(void) {
    LOG_INF("🔆 Brightness Control: Fixed Mode (85%)");
    
    zmk_widget_debug_status_set_text(&debug_widget, "🔆 Fixed Mode (CONFIG=n)");
    
    // Schedule a delayed message to avoid being overwritten
    static struct k_work_delayable debug_msg_work;
    k_work_init_delayable(&debug_msg_work, delayed_debug_msg);
    k_work_schedule(&debug_msg_work, K_MSEC(3000));  // Show after 3 seconds
    
    // Try to get PWM device safely
    const struct device *pwm_dev = NULL;
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
#endif
    
    if (pwm_dev && device_is_ready(pwm_dev)) {
        uint8_t brightness = 85;  // Default
#ifdef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB
        brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB;
#endif
        int ret = led_set_brightness(pwm_dev, 0, brightness);
        if (ret < 0) {
            LOG_WRN("Failed to set brightness: %d", ret);
        } else {
            LOG_INF("✅ Fixed brightness set to %d%%", brightness);
        }
    } else {
        LOG_INF("PWM device not found - using hardware default brightness");
    }
    
    return 0;  // Always succeed
}

SYS_INIT(brightness_control_init, APPLICATION, 90);

#else  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y

// Sensor mode implementation with actual ambient light control
static const struct device *pwm_dev;
static const struct device *sensor_dev;
static struct k_work_delayable brightness_update_work;
static struct k_work_delayable fade_work;

// Forward declarations for delayed debug functions
static void delayed_sensor_msg(struct k_work *work);
static void delayed_error_msg(struct k_work *work);

// Fade state tracking
static uint8_t current_brightness = 50;
static uint8_t target_brightness = 50;
static uint8_t fade_step_count = 0;
static uint8_t fade_total_steps = 10;

// Default configuration values
#ifndef CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#define CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS 20
#endif

#ifndef CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS_USB
#define CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS_USB 100
#endif

#ifndef CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD
#define CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD 100
#endif

#ifndef CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS
#define CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS 2000
#endif

#ifndef CONFIG_PROSPECTOR_BRIGHTNESS_FADE_DURATION_MS
#define CONFIG_PROSPECTOR_BRIGHTNESS_FADE_DURATION_MS 1000
#endif

#ifndef CONFIG_PROSPECTOR_BRIGHTNESS_FADE_STEPS
#define CONFIG_PROSPECTOR_BRIGHTNESS_FADE_STEPS 10
#endif

static uint8_t map_light_to_brightness(uint32_t light_value) {
    uint8_t min_brightness = CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS;
    uint8_t max_brightness = CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS_USB;
    uint32_t threshold = CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD;
    
    // Linear mapping: 0 -> min_brightness, threshold+ -> max_brightness
    if (light_value >= threshold) {
        return max_brightness;
    }
    
    // Calculate brightness as percentage between min and max
    uint32_t brightness_range = max_brightness - min_brightness;
    uint32_t scaled_brightness = (light_value * brightness_range) / threshold;
    
    return min_brightness + (uint8_t)scaled_brightness;
}

static void fade_work_handler(struct k_work *work) {
    if (!pwm_dev) {
        return;
    }
    
    if (current_brightness == target_brightness) {
        return; // Fade complete
    }
    
    fade_step_count++;
    
    // Calculate intermediate brightness value
    int brightness_diff = (int)target_brightness - (int)current_brightness;
    int step_change = (brightness_diff * fade_step_count) / fade_total_steps;
    uint8_t new_brightness = current_brightness + step_change;
    
    // Set brightness
    int ret = led_set_brightness(pwm_dev, 0, new_brightness);
    if (ret < 0) {
        LOG_WRN("Failed to set fade brightness: %d", ret);
    }
    
    // Check if fade is complete
    if (fade_step_count >= fade_total_steps || new_brightness == target_brightness) {
        current_brightness = target_brightness;
        LOG_DBG("✅ Fade complete: %u%%", current_brightness);
        return;
    }
    
    // Schedule next fade step
    uint32_t fade_interval = CONFIG_PROSPECTOR_BRIGHTNESS_FADE_DURATION_MS / fade_total_steps;
    k_work_schedule(&fade_work, K_MSEC(fade_interval));
}

static void start_brightness_fade(uint8_t new_target_brightness) {
    if (new_target_brightness == target_brightness) {
        return; // No change needed
    }
    
    // Cancel any ongoing fade
    k_work_cancel_delayable(&fade_work);
    
    // Set up new fade parameters
    target_brightness = new_target_brightness;
    fade_step_count = 0;
    fade_total_steps = CONFIG_PROSPECTOR_BRIGHTNESS_FADE_STEPS;
    
    LOG_DBG("🔄 Starting fade: %u%% -> %u%% (%u steps, %ums total)", 
            current_brightness, target_brightness, fade_total_steps, 
            CONFIG_PROSPECTOR_BRIGHTNESS_FADE_DURATION_MS);
    
    // Start immediate first step
    uint32_t fade_interval = CONFIG_PROSPECTOR_BRIGHTNESS_FADE_DURATION_MS / fade_total_steps;
    k_work_schedule(&fade_work, K_MSEC(fade_interval));
}

static void brightness_update_work_handler(struct k_work *work) {
    if (!sensor_dev || !pwm_dev) {
        return;
    }
    
    struct sensor_value light_val;
    int ret = sensor_sample_fetch(sensor_dev);
    if (ret < 0) {
        LOG_WRN("Failed to fetch sensor data: %d", ret);
        goto reschedule;
    }
    
    ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_LIGHT, &light_val);
    if (ret < 0) {
        LOG_WRN("Failed to get light sensor data: %d", ret);
        goto reschedule;
    }
    
    // Convert sensor value to simple integer (0-1000+ range expected)
    uint32_t light_level = (uint32_t)(light_val.val1 + (light_val.val2 / 1000000));
    uint8_t new_target_brightness = map_light_to_brightness(light_level);
    
    LOG_DBG("Light: %u -> Target Brightness: %u%%", light_level, new_target_brightness);
    
    // Start smooth fade to new brightness instead of immediate change
    start_brightness_fade(new_target_brightness);
    
reschedule:
    k_work_schedule(&brightness_update_work, K_MSEC(CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS));
}

// Delayed debug message functions
static void delayed_sensor_msg(struct k_work *work) {
    zmk_widget_debug_status_set_text(&debug_widget, "✅ SENSOR INIT CALLED");
}

static void delayed_error_msg(struct k_work *work) {
    zmk_widget_debug_status_set_text(&debug_widget, "❌ SENSOR INIT FAILED");
}

static int brightness_control_init(void) {
    LOG_INF("🌞 Brightness Control: Sensor Mode (4-pin connector, polling mode)");
    LOG_INF("📡 Using APDS9960 in polling mode - no INT pin required");
    
    // IMMEDIATE debug message - should show right away
    zmk_widget_debug_status_set_text(&debug_widget, "🌞 SENSOR INIT STARTED");
    
    // Also schedule immediate repeated message
    static struct k_work_delayable immediate_debug_work;
    k_work_init_delayable(&immediate_debug_work, delayed_sensor_msg);
    k_work_schedule(&immediate_debug_work, K_MSEC(100));  // Show after 100ms
    
    // Get PWM device safely
    pwm_dev = NULL;
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
#endif
    
    if (!pwm_dev || !device_is_ready(pwm_dev)) {
        LOG_ERR("PWM device not ready");
        return 0;
    }
    
    // Get sensor device safely
    sensor_dev = NULL;
#if DT_HAS_COMPAT_STATUS_OKAY(avago_apds9960) && IS_ENABLED(CONFIG_APDS9960)
    LOG_DBG("🔍 Device tree has APDS9960 definition, getting device...");
    sensor_dev = DEVICE_DT_GET_ONE(avago_apds9960);
    LOG_DBG("🔍 Sensor device pointer: %p", sensor_dev);
#else
    LOG_WRN("🔍 No APDS9960 device tree definition or CONFIG_APDS9960 disabled");
#endif
    
    if (!sensor_dev) {
        LOG_ERR("🔍 APDS9960 sensor device is NULL - device tree issue");
        zmk_widget_debug_status_set_text(&debug_widget, "❌ APDS9960: NULL Device");
        led_set_brightness(pwm_dev, 0, CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    zmk_widget_debug_status_set_text(&debug_widget, "🔍 APDS9960: Checking Ready...");
    
    if (!device_is_ready(sensor_dev)) {
        LOG_ERR("🔍 APDS9960 sensor not ready - 4-pin hardware or I2C issue");
        LOG_WRN("Falling back to fixed brightness mode");
        zmk_widget_debug_status_set_text(&debug_widget, "❌ APDS9960: Not Ready (I2C?)");
        
        // Schedule a delayed message to avoid being overwritten
        static struct k_work_delayable error_debug_work;
        k_work_init_delayable(&error_debug_work, delayed_error_msg);
        k_work_schedule(&error_debug_work, K_MSEC(3000));  // Show after 3 seconds
        
        // Set fallback brightness
        led_set_brightness(pwm_dev, 0, CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    LOG_INF("✅ APDS9960 sensor ready - 4-pin mode with polling (no INT pin)");
    zmk_widget_debug_status_set_text(&debug_widget, "✅ APDS9960: Ready (4-pin)");
    
    // Schedule a delayed message to avoid being overwritten
    static struct k_work_delayable sensor_debug_work;
    k_work_init_delayable(&sensor_debug_work, delayed_sensor_msg);
    k_work_schedule(&sensor_debug_work, K_MSEC(3000));  // Show after 3 seconds
    
    LOG_INF("📊 Sensor: Min=%u%%, Max=%u%%, Threshold=%u, Interval=%ums", 
            CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS,
            CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS_USB,
            CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD,
            CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS);
    LOG_INF("🔄 Fade: Duration=%ums, Steps=%u", 
            CONFIG_PROSPECTOR_BRIGHTNESS_FADE_DURATION_MS,
            CONFIG_PROSPECTOR_BRIGHTNESS_FADE_STEPS);
    
    // Initialize work queues
    k_work_init_delayable(&brightness_update_work, brightness_update_work_handler);
    k_work_init_delayable(&fade_work, fade_work_handler);
    
    // Set initial brightness state
    current_brightness = CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS;
    target_brightness = CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS;
    led_set_brightness(pwm_dev, 0, current_brightness);
    
    // Start brightness monitoring
    k_work_schedule(&brightness_update_work, K_MSEC(1000));  // Start after 1 second
    
    return 0;
}

SYS_INIT(brightness_control_init, APPLICATION, 70);  // Higher priority for sensor mode

#endif  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR