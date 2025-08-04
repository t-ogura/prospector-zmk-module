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

// Sensor value range for APDS9960 (raw 16-bit ADC counts)
#define SENSOR_MIN      0       // Minimum sensor reading (complete darkness)
#define SENSOR_MAX      65535   // Maximum sensor reading (16-bit ADC max)
#ifdef CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#define PWM_MIN         CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#else
#define PWM_MIN         10      // Minimum brightness (%) - keep display visible
#endif

#ifdef CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS
#define PWM_MAX         CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS
#else
#define PWM_MAX         100     // Maximum brightness (%)
#endif

// Alternative thresholds for more practical range (most indoor scenarios)
// APDS9960 typically outputs 0-5000 in normal indoor lighting
#define SENSOR_PRACTICAL_MIN    0       // Dark room
#define SENSOR_PRACTICAL_MAX    5000    // Bright indoor lighting

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
    if (!als_dev || !device_is_ready(als_dev)) {
        LOG_WRN("ALS device not ready");
        return;
    }
    
    // Use sensor_sample_fetch to get latest data
    int ret = sensor_sample_fetch(als_dev);
    if (ret < 0) {
        LOG_WRN("Failed to fetch ALS sample: %d", ret);
        // If sensor fails, maintain current brightness
        return;
    }
    
    struct sensor_value als_val = {0, 0};
    // Try to get ambient light value
    // APDS9960 should provide data on SENSOR_CHAN_LIGHT
    ret = sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &als_val);
    if (ret < 0) {
        LOG_WRN("Failed to get ambient light value: %d", ret);
        // Try alternative channel if primary fails
        ret = sensor_channel_get(als_dev, SENSOR_CHAN_RED, &als_val);
        if (ret < 0) {
            LOG_WRN("Failed to get any light value from APDS9960");
            return;
        }
        LOG_DBG("Using RED channel as fallback");
    }
    
    // Convert sensor value to brightness percentage
    // APDS9960 returns raw ADC counts, not lux directly
    // Typical range is 0-65535 for 16-bit ADC, but practical range is 0-5000
    int32_t light_level = als_val.val1;
    
    // Log raw value for debugging with more detail
    LOG_INF("🔆 APDS9960 raw light: %d (val2: %d)", als_val.val1, als_val.val2);
    LOG_INF("🔆 Range check: min=%d, max=%d, practical_max=%d", 
            SENSOR_PRACTICAL_MIN, SENSOR_PRACTICAL_MAX, SENSOR_PRACTICAL_MAX);
    
    uint8_t brightness;
    
    // Use practical range for better indoor lighting response
    // Clamp to practical range first
    if (light_level < SENSOR_PRACTICAL_MIN) {
        light_level = SENSOR_PRACTICAL_MIN;
    } else if (light_level > SENSOR_PRACTICAL_MAX) {
        light_level = SENSOR_PRACTICAL_MAX;
    }
    
    // Non-linear mapping for better perceptual response
    // Use square root curve for more sensitivity in dark conditions
    uint32_t normalized = ((uint32_t)(light_level - SENSOR_PRACTICAL_MIN) * 1000) / 
                         (SENSOR_PRACTICAL_MAX - SENSOR_PRACTICAL_MIN);
    
    // Apply square root curve (approximation using simple method)
    // This gives more resolution in dark conditions
    uint32_t sqrt_normalized = 0;
    uint32_t bit = 1 << 15; // Start with highest bit
    while (bit > normalized) bit >>= 2;
    while (bit != 0) {
        if (normalized >= sqrt_normalized + bit) {
            normalized -= sqrt_normalized + bit;
            sqrt_normalized = (sqrt_normalized >> 1) + bit;
        } else {
            sqrt_normalized >>= 1;
        }
        bit >>= 2;
    }
    
    // Scale to brightness range
    brightness = PWM_MIN + ((PWM_MAX - PWM_MIN) * sqrt_normalized) / 31; // sqrt(1000) ≈ 31
    
    // Apply brightness via LED API
    set_brightness_pwm(brightness);
    LOG_INF("💡 APDS9960: light=%d → brightness=%d%% (normalized=%d, sqrt=%d)", 
            light_level, brightness, normalized, sqrt_normalized);
}

static void brightness_work_handler(struct k_work *work) {
    update_brightness();
    
    // Schedule next update
#ifdef CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS
    k_work_schedule(&brightness_work, K_MSEC(CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS));
#else
    k_work_schedule(&brightness_work, K_SECONDS(2));
#endif
}

static int brightness_control_init(void) {
    // Initialize PWM LEDs device (using LED API)
    if (!device_is_ready(pwm_leds_dev)) {
        LOG_ERR("PWM LEDs device not ready");
        return -ENODEV;
    }
    
    // Initialize ALS device using direct node reference
    // APDS9960 is defined in the board overlay at I2C address 0x39
    als_dev = DEVICE_DT_GET(DT_NODELABEL(apds9960));
    if (!als_dev) {
        LOG_ERR("❌ APDS9960 device not found in device tree");
        LOG_WRN("Using fixed brightness: %d%%", CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    if (!device_is_ready(als_dev)) {
        LOG_ERR("❌ APDS9960 ambient light sensor NOT READY - hardware may be missing or not connected");
        LOG_WRN("Using fixed brightness: %d%%", CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    LOG_INF("✅ APDS9960 ambient light sensor READY - automatic brightness control enabled");
    LOG_INF("🔧 APDS9960 device name: %s", als_dev->name);
    
    // Configure APDS9960 for ambient light sensing
    // The sensor needs to be properly configured for ALS mode
    
    // Try to do an initial sensor read to verify it's working
    LOG_INF("🔧 Stabilizing sensor for 100ms...");
    k_msleep(100); // Give sensor time to stabilize
    
    int ret = sensor_sample_fetch(als_dev);
    if (ret == 0) {
        struct sensor_value test_val;
        ret = sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &test_val);
        if (ret == 0) {
            LOG_INF("📊 APDS9960 initial reading: %d (raw ADC value, expecting 0-65535)", test_val.val1);
            LOG_INF("📊 Practical range for indoor use: 0-5000");
        } else {
            LOG_WRN("Failed to get initial light value: %d", ret);
        }
    } else {
        LOG_WRN("Failed to fetch initial sample: %d", ret);
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