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

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "brightness_control.h"

// Only compile brightness control if sensor mode is DISABLED
#if !IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)

// Fixed brightness mode - state tracking
static const struct device *fixed_pwm_dev = NULL;
static uint8_t fixed_brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;

// API: Set manual brightness
void brightness_control_set_manual(uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    if (brightness < 10) brightness = 10;

    fixed_brightness = brightness;

    if (fixed_pwm_dev && device_is_ready(fixed_pwm_dev)) {
        led_set_brightness(fixed_pwm_dev, 0, brightness);
        LOG_INF("🔆 Manual brightness: %d%%", brightness);
    }
}

// API: Auto brightness not available in fixed mode
void brightness_control_set_auto(bool enabled) {
    LOG_WRN("🔆 Auto brightness not available (no sensor)");
}

// API: Get current brightness
uint8_t brightness_control_get_current(void) {
    return fixed_brightness;
}

// API: Check if auto is enabled (always false in fixed mode)
bool brightness_control_is_auto(void) {
    return false;
}

// Fixed brightness mode implementation - safe and simple
static int brightness_control_init(void) {
    LOG_INF("🔆 Brightness Control: Fixed Mode");

    // Try to get PWM device safely
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    fixed_pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
#endif

    if (fixed_pwm_dev && device_is_ready(fixed_pwm_dev)) {
        fixed_brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;
        int ret = led_set_brightness(fixed_pwm_dev, 0, fixed_brightness);
        if (ret < 0) {
            LOG_WRN("Failed to set brightness: %d", ret);
        } else {
            LOG_INF("✅ Fixed brightness set to %d%%", fixed_brightness);
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

static int brightness_control_init(void) {
    LOG_INF("🌞 Brightness Control: Sensor Mode");
    LOG_INF("⚠️  Sensor mode requires APDS9960 hardware and CONFIG_APDS9960=y");
    
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
    sensor_dev = DEVICE_DT_GET_ONE(avago_apds9960);
#endif
    
    if (!sensor_dev || !device_is_ready(sensor_dev)) {
        LOG_WRN("APDS9960 sensor not ready - check hardware connection and CONFIG_APDS9960=y");
        // Set fallback brightness
        led_set_brightness(pwm_dev, 0, CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    LOG_INF("✅ APDS9960 sensor ready - starting automatic brightness control with smooth fading");
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

// Sensor mode state
static bool auto_brightness_enabled = true;
static uint8_t manual_brightness_setting = 65;

// API: Set manual brightness
void brightness_control_set_manual(uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    if (brightness < 10) brightness = 10;

    manual_brightness_setting = brightness;

    if (!auto_brightness_enabled && pwm_dev && device_is_ready(pwm_dev)) {
        target_brightness = brightness;
        // Use fade for smooth transition
        k_work_schedule(&fade_work, K_NO_WAIT);
        LOG_INF("🔆 Manual brightness: %d%%", brightness);
    }
}

// API: Enable/disable auto brightness
void brightness_control_set_auto(bool enabled) {
    auto_brightness_enabled = enabled;

    if (enabled) {
        // Resume auto brightness updates
        k_work_schedule(&brightness_update_work, K_NO_WAIT);
        LOG_INF("🔆 Auto brightness enabled");
    } else {
        // Stop auto updates and apply manual setting
        k_work_cancel_delayable(&brightness_update_work);
        target_brightness = manual_brightness_setting;
        k_work_schedule(&fade_work, K_NO_WAIT);
        LOG_INF("🔆 Auto brightness disabled, manual: %d%%", manual_brightness_setting);
    }
}

// API: Get current brightness
uint8_t brightness_control_get_current(void) {
    return current_brightness;
}

// API: Check if auto is enabled
bool brightness_control_is_auto(void) {
    return auto_brightness_enabled;
}

SYS_INIT(brightness_control_init, APPLICATION, 90);

#endif  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR