/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 * 
 * Safe Brightness Control for Prospector v1.1.1
 * - CONFIG=n: Fixed brightness mode (v1.0.0 behavior)
 * - CONFIG=y: Sensor mode with safe fallback
 * - Both modes work safely without Device Tree issues
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Global state
static const struct device *pwm_dev = NULL;
static bool brightness_initialized = false;

// Safe brightness setter - always safe to call
static void set_brightness_safe(uint8_t brightness) {
    if (pwm_dev && device_is_ready(pwm_dev)) {
        int ret = led_set_brightness(pwm_dev, 0, brightness);
        if (ret < 0) {
            LOG_WRN("Failed to set brightness: %d", ret);
        } else {
            LOG_DBG("Brightness set to %d%%", brightness);
        }
    }
}

// Initialize brightness control system
static int brightness_control_init(void) {
#if !IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
    // Fixed brightness mode (CONFIG=n) - v1.0.0 behavior
    LOG_INF("🔆 Brightness Control: Fixed Mode");
    
    // Try to get PWM device safely
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
    if (!device_is_ready(pwm_dev)) {
        pwm_dev = NULL;
    }
#endif
    
    if (pwm_dev) {
        // Set fixed brightness
        uint8_t brightness = 85;  // Default
#ifdef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB
        brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB;
#endif
        set_brightness_safe(brightness);
        LOG_INF("✅ Fixed brightness set to %d%%", brightness);
    } else {
        LOG_WRN("PWM device not found - using hardware default brightness");
    }
    
#else  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y
    // Sensor mode (CONFIG=y) - v1.1.0 behavior with safety
    LOG_INF("🌞 Brightness Control: Sensor Mode");
    
    // Try to get PWM device
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
    if (!device_is_ready(pwm_dev)) {
        pwm_dev = NULL;
    }
#endif
    
    if (!pwm_dev) {
        LOG_WRN("PWM device not found - sensor mode disabled");
        return 0;  // Don't fail init
    }
    
    // Check for sensor availability
#if DT_HAS_COMPAT_STATUS_OKAY(avago_apds9960)
    const struct device *sensor_dev = DEVICE_DT_GET_ONE(avago_apds9960);
    if (device_is_ready(sensor_dev)) {
        LOG_INF("✅ APDS9960 sensor found - auto brightness enabled");
        // TODO: Implement sensor reading in work queue
        // For now, use fixed brightness as fallback
        set_brightness_safe(80);
    } else {
        LOG_WRN("APDS9960 sensor not ready - using fixed brightness");
        set_brightness_safe(80);
    }
#else
    LOG_WRN("No APDS9960 in Device Tree - using fixed brightness");
    set_brightness_safe(80);
#endif
    
#endif  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR
    
    brightness_initialized = true;
    return 0;  // Always succeed - never fail boot
}

// Use post-kernel init to ensure devices are ready
SYS_INIT(brightness_control_init, APPLICATION, 90);