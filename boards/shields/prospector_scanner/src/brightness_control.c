/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 * 
 * Simple and Safe Brightness Control for Prospector v1.1.1
 * - CONFIG=n: Fixed brightness mode (safe, no sensor required)
 * - CONFIG=y: APDS9960 sensor mode (requires sensor connection!)
 * - Clear separation to prevent Device Tree linking issues
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Fixed brightness mode implementation (safe default)
#if !IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)

static int brightness_control_init(void) {
    LOG_INF("🔆 Prospector Brightness: Fixed Mode");
    
    // Get PWM LEDs device safely with conditional Device Tree access
    const struct device *pwm_dev = NULL;
    
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
#endif
    
    if (!pwm_dev || !device_is_ready(pwm_dev)) {
        LOG_ERR("PWM LEDs device not found or not ready");
        // Continue without brightness control - don't fail initialization
        return 0;  // Return success to prevent boot failure
    }
    
    // Set fixed brightness
    uint8_t brightness = 80;  // Default 80%
#ifdef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB
    brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB;
#endif
    
    int ret = led_set_brightness(pwm_dev, 0, brightness); // Fixed channel 0 for backlight
    if (ret < 0) {
        LOG_ERR("Failed to set brightness: %d", ret);
        return ret;
    }
    
    LOG_INF("✅ Fixed brightness set to %d%%", brightness);
    return 0;
}

SYS_INIT(brightness_control_init, APPLICATION, 99); // Very low priority - init after everything else

#else // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y

// Sensor mode implementation (requires APDS9960 connection!)
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>

static struct k_work_delayable brightness_work;

static void update_brightness(void) {
    // Get devices with safe conditional Device Tree access
    const struct device *pwm_dev = NULL;
    const struct device *als_dev = NULL;
    
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(avago_apds9960)
    als_dev = DEVICE_DT_GET_ONE(avago_apds9960);
#endif

    if (!pwm_dev) {
        LOG_WRN("PWM LEDs device not found - skipping brightness update");
        return;
    }
    
    if (!als_dev) {
        LOG_WRN("APDS9960 sensor not found - using fixed brightness");
        // Fallback to fixed brightness
        if (device_is_ready(pwm_dev)) {
            int ret = led_set_brightness(pwm_dev, 0, 80);
            if (ret < 0) {
                LOG_ERR("Failed to set fixed brightness: %d", ret);
            }
        }
        return;
    }
    
    if (!device_is_ready(pwm_dev) || !device_is_ready(als_dev)) {
        LOG_ERR("Devices not ready - pwm:%s als:%s", 
               device_is_ready(pwm_dev) ? "OK" : "FAIL",
               device_is_ready(als_dev) ? "OK" : "FAIL");
        return;
    }
    
    // Read sensor
    struct sensor_value light;
    if (sensor_sample_fetch(als_dev) < 0 || 
        sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &light) < 0) {
        LOG_WRN("Failed to read ambient light sensor");
        return;
    }
    
    // Simple linear mapping: 0-200 sensor → 20-100% brightness
    int brightness = 20 + ((light.val1 * 80) / 200);
    if (brightness > 100) brightness = 100;
    if (brightness < 20) brightness = 20;
    
    // Set brightness
    int ret = led_set_brightness(pwm_dev, 0, brightness); // Fixed channel 0 for backlight
    if (ret >= 0) {
        LOG_DBG("Brightness: %d%% (light: %d)", brightness, light.val1);
    }
}

static void brightness_work_handler(struct k_work *work) {
    update_brightness();
    k_work_schedule(&brightness_work, K_SECONDS(2));
}

static int brightness_control_init(void) {
    LOG_INF("🌞 Prospector Brightness: Sensor Mode");
    
    // Initialize work queue safely
    k_work_init_delayable(&brightness_work, brightness_work_handler);
    
    // Start brightness control with delay - non-blocking approach
    k_work_schedule(&brightness_work, K_SECONDS(3));
    
    LOG_INF("✅ Sensor mode brightness control initialized successfully");
    return 0;  // Always return success - never fail boot
}

SYS_INIT(brightness_control_init, APPLICATION, 99); // Very low priority - init after everything else

#endif // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR