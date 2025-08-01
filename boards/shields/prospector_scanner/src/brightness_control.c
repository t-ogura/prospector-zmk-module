/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// LED driver for backlight control (following original Prospector approach)
static const struct device *pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);
#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

// Sensor value range from original Prospector (0-100, not raw ADC)
#define SENSOR_MIN      0       // Minimum sensor reading
#define SENSOR_MAX      100     // Maximum sensor reading  
#define PWM_MIN         10      // Minimum brightness (%) - keep display visible
#define PWM_MAX         100     // Maximum brightness (%)

static void set_brightness_pwm(uint8_t brightness_percent) {
    if (!device_is_ready(pwm_leds_dev)) {
        LOG_ERR("❌ PWM LEDs device not ready for brightness control");
        return;
    }
    
    // Use LED API like original Prospector
    int ret = led_set_brightness(pwm_leds_dev, DISP_BL, brightness_percent);
    
    if (ret < 0) {
        LOG_ERR("❌ Failed to set LED brightness: %d", ret);
    } else {
        LOG_DBG("✅ Backlight brightness: %d%%", brightness_percent);
    }
}

#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)

static const struct device *als_dev;
static struct k_work_delayable brightness_work;

static void update_brightness(void) {
    if (!als_dev) {
        return;
    }
    
    // Use sensor_sample_fetch like original Prospector
    int ret = sensor_sample_fetch(als_dev);
    if (ret < 0) {
        LOG_WRN("Failed to fetch ALS sample: %d", ret);
        return;
    }
    
    struct sensor_value als_val;
    // Try to get ambient light value first
    ret = sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &als_val);
    if (ret < 0) {
        LOG_WRN("Failed to get ambient light value: %d", ret);
        return;
    }
    
    // Convert sensor value to brightness percentage
    // APDS9960 returns raw ADC counts, not lux directly
    // Typical range is 0-65535 for 16-bit ADC
    int32_t light_level = als_val.val1;
    
    // Log raw value for debugging
    LOG_DBG("APDS9960 raw light value: %d (val2: %d)", als_val.val1, als_val.val2);
    
    uint8_t brightness;
    
    // Original Prospector uses 0-100 range for sensor values
    // Handle invalid readings
    if (light_level < SENSOR_MIN) {
        brightness = PWM_MIN;  // Default to minimum brightness
    } else if (light_level > SENSOR_MAX) {
        brightness = PWM_MAX;  // Cap at maximum
    } else {
        // Linear mapping like original Prospector
        brightness = PWM_MIN + ((PWM_MAX - PWM_MIN) * 
                               (light_level - SENSOR_MIN)) / (SENSOR_MAX - SENSOR_MIN);
    }
    
    // Apply brightness via LED API
    set_brightness_pwm(brightness);
    LOG_INF("💡 APDS9960: %d → %d%% brightness", light_level, brightness);
}

static void brightness_work_handler(struct k_work *work) {
    update_brightness();
    
    // Schedule next update
    k_work_schedule(&brightness_work, K_SECONDS(2));
}

static int brightness_control_init(void) {
    // Initialize PWM LEDs device (using LED API)
    if (!device_is_ready(pwm_leds_dev)) {
        LOG_ERR("PWM LEDs device not ready");
        return -ENODEV;
    }
    
    // Initialize ALS device
    als_dev = DEVICE_DT_GET(DT_ALIAS(als));
    if (!device_is_ready(als_dev)) {
        LOG_ERR("❌ APDS9960 ambient light sensor NOT READY - hardware may be missing or not connected");
        LOG_WRN("Using fixed brightness: %d%%", CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    LOG_INF("✅ APDS9960 ambient light sensor READY - automatic brightness control enabled");
    
    // Try to do an initial sensor read to verify it's working
    if (sensor_sample_fetch(als_dev) == 0) {
        struct sensor_value test_val;
        if (sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &test_val) == 0) {
            LOG_INF("📊 APDS9960 initial reading: %d (expecting 0-100 range)", test_val.val1);
        }
    }
    
    // Initialize work queue
    k_work_init_delayable(&brightness_work, brightness_work_handler);
    
    // Start brightness monitoring
    k_work_schedule(&brightness_work, K_SECONDS(1));
    
    return 0;
}

#else // !CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

static int brightness_control_init(void) {
    // Initialize PWM LEDs device (using LED API)
    if (!device_is_ready(pwm_leds_dev)) {
        LOG_ERR("PWM LEDs device not ready");
        return -ENODEV;
    }
    
    // Set fixed brightness
    set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    LOG_INF("🔆 Fixed brightness mode: %d%% (ambient light sensor disabled)", 
            CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    return 0;
}

#endif // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

SYS_INIT(brightness_control_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);