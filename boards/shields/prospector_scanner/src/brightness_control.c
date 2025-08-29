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

// Sensor mode implementation - requires hardware setup by user
static int brightness_control_init(void) {
    LOG_INF("🌞 Brightness Control: Sensor Mode");
    LOG_INF("⚠️  Sensor mode requires APDS9960 hardware and CONFIG_APDS9960=y");
    LOG_INF("⚠️  This is advanced functionality - ensure hardware is properly connected");
    
    // Get devices
    const struct device *pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
    const struct device *sensor_dev = DEVICE_DT_GET_ONE(avago_apds9960);
    
    if (!device_is_ready(pwm_dev)) {
        LOG_ERR("PWM device not ready");
        return 0;
    }
    
    if (!device_is_ready(sensor_dev)) {
        LOG_WRN("APDS9960 sensor not ready - check hardware connection");
        // Set fallback brightness
        led_set_brightness(pwm_dev, 0, 80);
        return 0;
    }
    
    LOG_INF("✅ Both PWM and APDS9960 ready - sensor mode enabled");
    // Set initial brightness
    led_set_brightness(pwm_dev, 0, 80);
    
    return 0;  // Always succeed
}

SYS_INIT(brightness_control_init, APPLICATION, 90);

#endif  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR