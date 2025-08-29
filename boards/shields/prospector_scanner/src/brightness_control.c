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

// Only compile brightness control if sensor mode is DISABLED
#if !IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)

// Fixed brightness mode implementation - safe and simple
static int brightness_control_init(void) {
    LOG_INF("🔆 Brightness Control: Fixed Mode (85%)");
    
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
    uint8_t target_brightness = map_light_to_brightness(light_level);
    
    ret = led_set_brightness(pwm_dev, 0, target_brightness);
    if (ret < 0) {
        LOG_WRN("Failed to set brightness: %d", ret);
    } else {
        LOG_DBG("Light: %u -> Brightness: %u%%", light_level, target_brightness);
    }
    
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
    
    LOG_INF("✅ APDS9960 sensor ready - starting automatic brightness control");
    LOG_INF("📊 Settings: Min=%u%%, Max=%u%%, Threshold=%u, Interval=%ums", 
            CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS,
            CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS_USB,
            CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD,
            CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS);
    
    // Initialize work queue
    k_work_init_delayable(&brightness_update_work, brightness_update_work_handler);
    
    // Set initial brightness
    led_set_brightness(pwm_dev, 0, CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS);
    
    // Start brightness monitoring
    k_work_schedule(&brightness_update_work, K_MSEC(1000));  // Start after 1 second
    
    return 0;
}

SYS_INIT(brightness_control_init, APPLICATION, 90);

#endif  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR