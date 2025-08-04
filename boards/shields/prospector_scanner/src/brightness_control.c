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
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include "debug_status_widget.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Global debug widget for sensor status display
extern struct zmk_widget_debug_status debug_widget;

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
    uint8_t brightness = PWM_MIN;  // Initialize to minimum brightness
    
    // Handle invalid/error readings
    if (light_level < SENSOR_MIN) {
        brightness = PWM_MIN;  // Default to minimum brightness on error
    } else if (light_level > SENSOR_MAX) {
        light_level = SENSOR_MAX;  // Clamp to maximum
        brightness = PWM_MAX;  // Set to maximum brightness
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
    
    // Debug display: Show current light level and brightness
    char status_buf[64];
    snprintf(status_buf, sizeof(status_buf), "L:%d B:%d%%", light_level, brightness);
    zmk_widget_debug_status_set_text(&debug_widget, status_buf);
    zmk_widget_debug_status_set_visible(&debug_widget, true);
}

static void brightness_work_handler(struct k_work *work) {
    update_brightness();
    
    // Schedule next update - continuous monitoring
#ifdef CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS
    k_work_schedule(&brightness_work, K_MSEC(CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS));
#else
    k_work_schedule(&brightness_work, K_SECONDS(2));
#endif
}

static int brightness_control_init(void) {
    LOG_INF("🚀 brightness_control_init STARTED (ALS enabled)");
    printk("BRIGHTNESS: brightness_control_init called (ALS mode)\n");
    
    // Initialize PWM LEDs device (using LED API)
    if (!device_is_ready(pwm_leds_dev)) {
        LOG_ERR("PWM LEDs device not ready");
        return -ENODEV;
    }
    
    LOG_INF("✅ PWM LEDs device ready");
    
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
        
        // Debug display: Show sensor not ready status (persistent)
        zmk_widget_debug_status_set_text(&debug_widget, "ALS: Device Not Ready");
        zmk_widget_debug_status_set_visible(&debug_widget, true);
        // No auto-hide - keep visible to show problem
        
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
            
            // Debug display: Show sensor working status (will update continuously)
            char status_buf[64];
            snprintf(status_buf, sizeof(status_buf), "ALS: OK (%d)", test_val.val1);
            zmk_widget_debug_status_set_text(&debug_widget, status_buf);
            zmk_widget_debug_status_set_visible(&debug_widget, true);
        } else {
            LOG_WRN("Failed to get initial light value: %d", ret);
            printk("BRIGHTNESS: Failed to get light value, error %d\n", ret);
            
            // Try alternative channels
            ret = sensor_channel_get(als_dev, SENSOR_CHAN_RED, &test_val);
            printk("BRIGHTNESS: RED channel test returned %d\n", ret);
            if (ret == 0) {
                printk("BRIGHTNESS: RED channel value: %d\n", test_val.val1);
                LOG_INF("✅ RED channel working as fallback: %d", test_val.val1);
                
                // Debug display: Show fallback channel working
                char status_buf[64];
                snprintf(status_buf, sizeof(status_buf), "ALS: RED Ch (%d)", test_val.val1);
                zmk_widget_debug_status_set_text(&debug_widget, status_buf);
                zmk_widget_debug_status_set_visible(&debug_widget, true);
            } else {
                // Debug display: Show channel read failed (persistent error)
                char status_buf[64];
                snprintf(status_buf, sizeof(status_buf), "ALS: Ch Read Fail (%d)", ret);
                zmk_widget_debug_status_set_text(&debug_widget, status_buf);
                zmk_widget_debug_status_set_visible(&debug_widget, true);
            }
        }
    } else {
        LOG_WRN("Failed to fetch initial sample: %d", ret);
        printk("BRIGHTNESS: sensor_sample_fetch FAILED with error %d\n", ret);
        printk("BRIGHTNESS: This suggests I2C communication problem or sensor not connected\n");
        
        // Debug display: Show I2C communication failed (persistent critical error)
        char status_buf[64];
        snprintf(status_buf, sizeof(status_buf), "ALS: I2C Fail (%d)", ret);
        zmk_widget_debug_status_set_text(&debug_widget, status_buf);
        zmk_widget_debug_status_set_visible(&debug_widget, true);
    }
    
    // Initialize work queue
    k_work_init_delayable(&brightness_work, brightness_work_handler);
    
    // Force debug widget to be visible with test message
    LOG_INF("🔧 About to access debug widget...");
    printk("BRIGHTNESS: Accessing debug widget\n");
    
    zmk_widget_debug_status_set_visible(&debug_widget, true);
    zmk_widget_debug_status_set_text(&debug_widget, "ALS: INIT TEST");
    LOG_INF("🎯 Forced debug widget visible with test message");
    
    // Start brightness monitoring with delay to ensure display is ready
    k_work_schedule(&brightness_work, K_SECONDS(3));
    
    return 0;
}

#else // !CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

static int brightness_control_init(void) {
    LOG_INF("🚀 brightness_control_init STARTED (Fixed brightness mode)");
    printk("BRIGHTNESS: brightness_control_init called (Fixed mode)\n");
    
    // Initialize PWM LEDs device (using LED API)
    if (!device_is_ready(pwm_leds_dev)) {
        LOG_ERR("PWM LEDs device not ready");
        return -ENODEV;
    }
    
    // Set fixed brightness
    set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    LOG_INF("🔆 Fixed brightness mode: %d%% (ambient light sensor disabled)", 
            CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
            
    // Test debug widget access even in fixed mode
    LOG_INF("🔧 Testing debug widget access in fixed mode...");
    zmk_widget_debug_status_set_text(&debug_widget, "ALS: DISABLED");
    zmk_widget_debug_status_set_visible(&debug_widget, true);
    LOG_INF("🔧 Debug widget should show ALS: DISABLED");
    
    return 0;
}

#endif // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

// Try different initialization approaches
SYS_INIT(brightness_control_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

// Also try a delayed work approach in case SYS_INIT isn't working
static struct k_work_delayable delayed_init_work;

static void delayed_init_work_handler(struct k_work *work) {
    LOG_INF("🔥 DELAYED INIT WORK EXECUTED!");
    printk("BRIGHTNESS: Delayed init work executed\n");
    
    // Now execute the actual brightness control initialization from here
    LOG_INF("🔧 Executing brightness control logic from delayed work...");
    
    // Check PWM device
    if (!device_is_ready(pwm_leds_dev)) {
        zmk_widget_debug_status_set_text(&debug_widget, "PWM NOT READY");
        LOG_ERR("PWM LEDs device not ready in delayed work");
        return;
    }
    
#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)
    LOG_INF("🔧 ALS mode detected in delayed work");
    
    // Try to get APDS9960 device
    const struct device *als_dev = DEVICE_DT_GET_ONE(avago_apds9960);
    if (!als_dev) {
        zmk_widget_debug_status_set_text(&debug_widget, "ALS: No Device");
        LOG_ERR("APDS9960 device not found");
        return;
    }
    
    if (!device_is_ready(als_dev)) {
        LOG_ERR("APDS9960 device not ready - investigating I2C status");
        
        // Check I2C bus status
        const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
        if (!i2c_dev) {
            zmk_widget_debug_status_set_text(&debug_widget, "ALS: No I2C Bus");
            LOG_ERR("I2C0 bus device not found");
            return;
        }
        
        if (!device_is_ready(i2c_dev)) {
            zmk_widget_debug_status_set_text(&debug_widget, "ALS: I2C Not Ready");
            LOG_ERR("I2C0 bus not ready");
            return;
        }
        
        LOG_INF("I2C0 bus is ready, but APDS9960 device_is_ready() failed");
        
        // Perform comprehensive I2C bus scan to find any responding devices
        LOG_INF("Performing comprehensive I2C bus scan...");
        bool found_any_device = false;
        
        // Scan common I2C addresses
        uint8_t test_addresses[] = {0x39, 0x29, 0x49, 0x23, 0x44, 0x45, 0x48, 0x4A, 0x53, 0x68, 0x76, 0x77};
        int num_addresses = sizeof(test_addresses) / sizeof(test_addresses[0]);
        
        for (int i = 0; i < num_addresses; i++) {
            uint8_t addr = test_addresses[i];
            uint8_t test_data = 0;
            int scan_ret = i2c_read(i2c_dev, &test_data, 1, addr);
            
            if (scan_ret == 0) {
                LOG_INF("✅ Device found at I2C address 0x%02X", addr);
                found_any_device = true;
                if (addr == 0x39) {
                    LOG_INF("🎯 APDS9960 found at expected address 0x39!");
                }
            }
        }
        
        if (!found_any_device) {
            LOG_WRN("❌ No I2C devices found on bus - possible hardware issue");
            zmk_widget_debug_status_set_text(&debug_widget, "ALS: No I2C Devices");
        } else {
            // Try specific APDS9960 register read (WHO_AM_I register at 0x92)
            uint8_t who_am_i = 0;
            int who_ret = i2c_reg_read_byte(i2c_dev, 0x39, 0x92, &who_am_i);
            
            if (who_ret == 0) {
                LOG_INF("✅ APDS9960 WHO_AM_I register: 0x%02X (expected: 0xAB)", who_am_i);
                if (who_am_i == 0xAB) {
                    zmk_widget_debug_status_set_text(&debug_widget, "ALS: ID OK, Init Fail");
                } else {
                    char id_buf[32];
                    snprintf(id_buf, sizeof(id_buf), "ALS: Wrong ID 0x%02X", who_am_i);
                    zmk_widget_debug_status_set_text(&debug_widget, id_buf);
                }
            } else {
                LOG_INF("❌ Failed to read APDS9960 WHO_AM_I register: %d", who_ret);
                zmk_widget_debug_status_set_text(&debug_widget, "ALS: Reg Read Fail");
            }
        }
        
        LOG_INF("Hardware check complete - device_is_ready() failed");
        return;
    }
    
    // Test sensor reading
    int ret = sensor_sample_fetch(als_dev);
    if (ret < 0) {
        char error_buf[32];
        snprintf(error_buf, sizeof(error_buf), "ALS: I2C Err %d", ret);
        zmk_widget_debug_status_set_text(&debug_widget, error_buf);
        LOG_ERR("APDS9960 sample fetch failed: %d", ret);
        return;
    }
    
    struct sensor_value als_val;
    ret = sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &als_val);
    if (ret < 0) {
        char error_buf[32];
        snprintf(error_buf, sizeof(error_buf), "ALS: Ch Err %d", ret);
        zmk_widget_debug_status_set_text(&debug_widget, error_buf);
        LOG_ERR("APDS9960 channel get failed: %d", ret);
        return;
    }
    
    // Success! Show sensor value
    char success_buf[32];
    snprintf(success_buf, sizeof(success_buf), "ALS: OK (%d)", als_val.val1);
    zmk_widget_debug_status_set_text(&debug_widget, success_buf);
    LOG_INF("✅ APDS9960 working: %d", als_val.val1);
    
#else
    LOG_INF("🔧 Fixed brightness mode detected in delayed work");
    zmk_widget_debug_status_set_text(&debug_widget, "ALS: Disabled");
#endif
}

// Also initialize via delayed work
static int delayed_brightness_init(void) {
    LOG_INF("🔥 Setting up delayed brightness init...");
    printk("BRIGHTNESS: Setting up delayed init work\n");
    
    k_work_init_delayable(&delayed_init_work, delayed_init_work_handler);
    k_work_schedule(&delayed_init_work, K_SECONDS(5));
    
    return 0;
}

SYS_INIT(delayed_brightness_init, POST_KERNEL, 99);