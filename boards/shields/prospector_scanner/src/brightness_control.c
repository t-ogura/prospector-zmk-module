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

// Original Prospector sensor value range (matches original implementation)
#define SENSOR_MIN      0       // Minimum sensor reading
#define SENSOR_MAX      100     // Maximum sensor reading (original Prospector range)
#ifdef CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#define PWM_MIN         CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#else
#define PWM_MIN         1       // Minimum brightness (%) - keep display visible (original: 1)
#endif

#ifdef CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS
#define PWM_MAX         CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS
#else
#define PWM_MAX         100     // Maximum brightness (%)
#endif

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
    
    // Convert sensor value to brightness percentage (original Prospector method)
    int32_t light_level = als_val.val1;
    
    // Log raw value for debugging
    LOG_INF("🔆 APDS9960 light level: %d (expecting 0-100 range)", light_level);
    printk("BRIGHTNESS: light=%d (range 0-100)\n", light_level);
    
    // Original Prospector linear mapping function
    uint8_t brightness;
    
    // Handle invalid/error readings
    if (light_level < SENSOR_MIN) {
        brightness = PWM_MIN;  // Default to minimum brightness on error
    } else if (light_level > SENSOR_MAX) {
        light_level = SENSOR_MAX;  // Clamp to maximum
    } else {
        // Linear mapping (original Prospector method)
        brightness = (uint8_t)(
            PWM_MIN + ((PWM_MAX - PWM_MIN) *
            (light_level - SENSOR_MIN)) / (SENSOR_MAX - SENSOR_MIN)
        );
    }
    
    // Apply brightness via LED API
    set_brightness_pwm(brightness);
    LOG_INF("💡 APDS9960: light=%d → brightness=%d%% (linear mapping)", 
            light_level, brightness);
    
    // Also use printk for final result
    printk("BRIGHTNESS: %d -> %d%%\n", light_level, brightness);
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
    
    /*
     * Visual Debug Patterns for APDS9960 Sensor Status:
     * 
     * 1 long flash (1s):           ✅ Sensor working successfully  
     * 2 double flashes:            ⚠️  Sensor found but channel read failed
     * 3 quick flashes (200ms):     ❌ Sensor not ready (hardware issue)
     * 5 slow flashes (500ms):      ❌ I2C communication failed
     * 
     * Hardware Requirements:
     * - APDS9960 connected to I2C0 (SDA=D4/P0.04, SCL=D5/P0.05)  
     * - I2C address: 0x39
     * - VCC: 3.3V, GND: Ground
     * - Optional INT: D2/P0.28 (with pull-up)
     */
    
    // Initialize ALS device using direct node reference
    // APDS9960 is defined in the board overlay at I2C address 0x39
    printk("BRIGHTNESS: Looking for APDS9960 device in device tree...\n");
    
    // Use original Prospector method: get by compatible string
    als_dev = DEVICE_DT_GET_ONE(avago_apds9960);
    if (!als_dev) {
        LOG_ERR("❌ APDS9960 device not found by compatible 'avago,apds9960'");
        LOG_WRN("Using fixed brightness: %d%%", CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        printk("BRIGHTNESS: APDS9960 device not found by compatible, using fixed brightness\n");
        set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    printk("BRIGHTNESS: APDS9960 device found, checking if ready...\n");
    
    if (!device_is_ready(als_dev)) {
        LOG_ERR("❌ APDS9960 ambient light sensor NOT READY - hardware may be missing or not connected");
        LOG_WRN("Using fixed brightness: %d%%", CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        printk("BRIGHTNESS: APDS9960 device not ready (I2C communication failed?)\n");
        printk("BRIGHTNESS: Check hardware connections - SDA to D4, SCL to D5, VCC to 3.3V, GND to GND\n");
        
        // Visual debug: Flash display to indicate sensor not ready
        // Pattern: 3 quick flashes = sensor not ready (hardware issue)
        for (int i = 0; i < 3; i++) {
            set_brightness_pwm(100);
            k_msleep(200);
            set_brightness_pwm(10);
            k_msleep(200);
        }
        
        set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    LOG_INF("✅ APDS9960 ambient light sensor READY - automatic brightness control enabled");
    LOG_INF("🔧 APDS9960 device name: %s", als_dev->name);
    
    // Also use printk for easier debugging
    printk("BRIGHTNESS: APDS9960 sensor ready, name=%s\n", als_dev->name);
    
    // Check I2C communication with sensor
    printk("BRIGHTNESS: Testing I2C communication at address 0x39\n");
    
    // Configure APDS9960 for ambient light sensing
    // The sensor needs to be properly configured for ALS mode
    
    // Try to do an initial sensor read to verify it's working
    LOG_INF("🔧 Stabilizing sensor for 100ms...");
    k_msleep(100); // Give sensor time to stabilize
    
    int ret = sensor_sample_fetch(als_dev);
    printk("BRIGHTNESS: sensor_sample_fetch returned %d\n", ret);
    
    if (ret == 0) {
        struct sensor_value test_val;
        ret = sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &test_val);
        printk("BRIGHTNESS: sensor_channel_get(LIGHT) returned %d\n", ret);
        
        if (ret == 0) {
            LOG_INF("📊 APDS9960 initial reading: %d (original Prospector expects 0-100)", test_val.val1);
            printk("BRIGHTNESS: Initial reading SUCCESS: %d\n", test_val.val1);
            
            // Visual debug: 1 long flash = sensor working successfully
            set_brightness_pwm(100);
            k_msleep(1000);
            set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        } else {
            LOG_WRN("Failed to get initial light value: %d", ret);
            printk("BRIGHTNESS: Failed to get light value, error %d\n", ret);
            
            // Visual debug: 2 double flashes = sensor found but channel read failed
            for (int i = 0; i < 2; i++) {
                set_brightness_pwm(100);
                k_msleep(150);
                set_brightness_pwm(10);
                k_msleep(150);
                set_brightness_pwm(100);
                k_msleep(150);
                set_brightness_pwm(10);
                k_msleep(300);
            }
            
            // Try alternative channels
            ret = sensor_channel_get(als_dev, SENSOR_CHAN_RED, &test_val);
            printk("BRIGHTNESS: RED channel test returned %d\n", ret);
            if (ret == 0) {
                printk("BRIGHTNESS: RED channel value: %d\n", test_val.val1);
                LOG_INF("✅ RED channel working as fallback: %d", test_val.val1);
            }
        }
    } else {
        LOG_WRN("Failed to fetch initial sample: %d", ret);
        printk("BRIGHTNESS: sensor_sample_fetch FAILED with error %d\n", ret);
        printk("BRIGHTNESS: This suggests I2C communication problem or sensor not connected\n");
        
        // Visual debug: 5 slow flashes = I2C communication failed
        for (int i = 0; i < 5; i++) {
            set_brightness_pwm(100);
            k_msleep(500);
            set_brightness_pwm(10);
            k_msleep(500);
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