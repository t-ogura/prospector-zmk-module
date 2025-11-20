/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Safe Brightness Control for Prospector v1.1.2
 * - CONFIG=n: Fixed brightness mode only
 * - CONFIG=y: Direct I2C sensor mode (no interrupt pin required)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/i2c.h>
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

// ========== Direct I2C APDS9960 Implementation ==========
// This bypasses the Zephyr driver which requires an interrupt pin

// APDS9960 I2C Address
#define APDS9960_I2C_ADDR       0x39

// APDS9960 Register Addresses
#define APDS9960_ENABLE_REG     0x80
#define APDS9960_ATIME_REG      0x81
#define APDS9960_CONTROL_REG    0x8F
#define APDS9960_ID_REG         0x92
#define APDS9960_STATUS_REG     0x93
#define APDS9960_CDATAL_REG     0x94
#define APDS9960_CDATAH_REG     0x95
#define APDS9960_AICLEAR_REG    0xE7

// APDS9960 Enable Register bits
#define APDS9960_ENABLE_PON     0x01  // Power ON
#define APDS9960_ENABLE_AEN     0x02  // ALS Enable

// APDS9960 Status Register bits
#define APDS9960_STATUS_AVALID  0x01  // ALS data valid

// APDS9960 Chip IDs
#define APDS9960_ID_1           0xAB
#define APDS9960_ID_2           0x9C

// APDS9960 ALS Gain values
#define APDS9960_AGAIN_1X       0x00
#define APDS9960_AGAIN_4X       0x01
#define APDS9960_AGAIN_16X      0x02
#define APDS9960_AGAIN_64X      0x03

// Default ADC integration time (219 = ~103ms)
#define APDS9960_DEFAULT_ATIME  219

// Sensor mode state
static const struct device *pwm_dev = NULL;
static const struct device *i2c_dev = NULL;
static struct k_work_delayable brightness_update_work;
static struct k_work_delayable fade_work;

// Fade state tracking
static uint8_t current_brightness = 50;
static uint8_t target_brightness = 50;
static uint8_t fade_step_count = 0;
static uint8_t fade_total_steps = 10;
static bool sensor_available = false;

// Auto/manual mode state
static bool auto_brightness_enabled = true;
static uint8_t manual_brightness_setting = 65;

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

// I2C Helper functions
static int apds9960_read_reg(uint8_t reg, uint8_t *val) {
    if (!i2c_dev) return -ENODEV;
    return i2c_reg_read_byte(i2c_dev, APDS9960_I2C_ADDR, reg, val);
}

static int apds9960_write_reg(uint8_t reg, uint8_t val) {
    if (!i2c_dev) return -ENODEV;
    return i2c_reg_write_byte(i2c_dev, APDS9960_I2C_ADDR, reg, val);
}

static int apds9960_read_word(uint8_t reg, uint16_t *val) {
    if (!i2c_dev) return -ENODEV;
    uint8_t data[2];
    int ret = i2c_burst_read(i2c_dev, APDS9960_I2C_ADDR, reg, data, 2);
    if (ret == 0) {
        *val = (uint16_t)data[0] | ((uint16_t)data[1] << 8);  // Little-endian
    }
    return ret;
}

// Initialize APDS9960 for ALS-only operation (no interrupt)
static int apds9960_init_als(void) {
    uint8_t chip_id;
    int ret;

    // Check chip ID
    ret = apds9960_read_reg(APDS9960_ID_REG, &chip_id);
    if (ret < 0) {
        LOG_ERR("Failed to read APDS9960 ID: %d", ret);
        return ret;
    }

    if (chip_id != APDS9960_ID_1 && chip_id != APDS9960_ID_2) {
        LOG_ERR("Invalid APDS9960 chip ID: 0x%02X", chip_id);
        return -ENODEV;
    }

    LOG_INF("✅ APDS9960 detected (ID: 0x%02X)", chip_id);

    // Disable everything first
    ret = apds9960_write_reg(APDS9960_ENABLE_REG, 0x00);
    if (ret < 0) return ret;

    // Clear any pending interrupts
    ret = apds9960_write_reg(APDS9960_AICLEAR_REG, 0x00);
    if (ret < 0) return ret;

    // Set ADC integration time
    ret = apds9960_write_reg(APDS9960_ATIME_REG, APDS9960_DEFAULT_ATIME);
    if (ret < 0) return ret;

    // Set ALS gain (4x for good indoor sensitivity)
    ret = apds9960_write_reg(APDS9960_CONTROL_REG, APDS9960_AGAIN_4X);
    if (ret < 0) return ret;

    // Enable power and ALS
    ret = apds9960_write_reg(APDS9960_ENABLE_REG, APDS9960_ENABLE_PON | APDS9960_ENABLE_AEN);
    if (ret < 0) return ret;

    LOG_INF("✅ APDS9960 ALS initialized (polling mode)");
    return 0;
}

// Read ambient light value (Clear channel)
static int apds9960_read_light(uint16_t *light_val) {
    uint8_t status;
    int ret;

    // Check if data is valid
    ret = apds9960_read_reg(APDS9960_STATUS_REG, &status);
    if (ret < 0) return ret;

    if (!(status & APDS9960_STATUS_AVALID)) {
        // Data not ready yet
        return -EAGAIN;
    }

    // Read Clear channel (ambient light)
    ret = apds9960_read_word(APDS9960_CDATAL_REG, light_val);
    if (ret < 0) return ret;

    return 0;
}

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
    if (!sensor_available || !pwm_dev) {
        goto reschedule;
    }

    if (!auto_brightness_enabled) {
        // Auto mode disabled, don't read sensor
        goto reschedule;
    }

    uint16_t light_val = 0;
    int ret = apds9960_read_light(&light_val);

    if (ret == -EAGAIN) {
        // Data not ready, try again soon
        k_work_schedule(&brightness_update_work, K_MSEC(100));
        return;
    }

    if (ret < 0) {
        LOG_WRN("Failed to read light sensor: %d", ret);
        goto reschedule;
    }

    uint8_t new_target_brightness = map_light_to_brightness(light_val);

    LOG_DBG("🌞 Light: %u -> Brightness: %u%%", light_val, new_target_brightness);

    // Start smooth fade to new brightness
    start_brightness_fade(new_target_brightness);

reschedule:
    k_work_schedule(&brightness_update_work, K_MSEC(CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS));
}

static int brightness_control_init(void) {
    LOG_INF("🌞 Brightness Control: Direct I2C Sensor Mode");

    // Get PWM device
    pwm_dev = NULL;
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
    pwm_dev = DEVICE_DT_GET_ONE(pwm_leds);
#endif

    if (!pwm_dev || !device_is_ready(pwm_dev)) {
        LOG_ERR("PWM device not ready");
        return 0;
    }

    // Get I2C device (from device tree)
    i2c_dev = NULL;
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c0), okay)
    i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
#endif

    if (!i2c_dev || !device_is_ready(i2c_dev)) {
        LOG_WRN("I2C device not ready - using fixed brightness");
        led_set_brightness(pwm_dev, 0, CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }

    // Initialize APDS9960 for ALS operation
    int ret = apds9960_init_als();
    if (ret < 0) {
        LOG_WRN("APDS9960 init failed - using fixed brightness");
        led_set_brightness(pwm_dev, 0, CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }

    sensor_available = true;

    LOG_INF("✅ Direct I2C brightness control ready");
    LOG_INF("📊 Settings: Min=%u%%, Max=%u%%, Threshold=%u, Interval=%ums",
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

    // Start brightness monitoring after 1 second
    k_work_schedule(&brightness_update_work, K_MSEC(1000));

    return 0;
}

// API: Set manual brightness
void brightness_control_set_manual(uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    if (brightness < 10) brightness = 10;

    manual_brightness_setting = brightness;

    if (!auto_brightness_enabled && pwm_dev && device_is_ready(pwm_dev)) {
        target_brightness = brightness;
        fade_step_count = 0;
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
        // Apply manual setting
        target_brightness = manual_brightness_setting;
        fade_step_count = 0;
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