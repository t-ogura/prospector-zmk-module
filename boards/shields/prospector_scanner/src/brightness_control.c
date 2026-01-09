/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Thread-Safe Brightness Control for Prospector v2.0
 * - Work Queue context: Sensor reading only, sends messages
 * - Main thread context: All PWM access happens in scanner_display.c
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "brightness_control.h"
#include "scanner_message.h"

// Auto brightness configuration defaults
#ifndef CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS
#define CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS 5   // 5% minimum brightness in dark
#endif
#ifndef CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS
#define CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS 100 // 100% maximum brightness
#endif
#ifndef CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD
#define CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD 500 // Light value for max brightness
#endif
#ifndef CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS
#define CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS 1000 // 1 second update interval
#endif

// Only compile sensor code if enabled
#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)

// ========== Direct I2C APDS9960 Implementation ==========

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

// Sensor state
static const struct device *i2c_dev = NULL;
static struct k_work_delayable brightness_sensor_work;
static bool sensor_available = false;
static bool auto_brightness_enabled = true;

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

// API: Get I2C device (main thread only!)
const struct device *brightness_control_get_i2c_dev(void) {
    return i2c_dev;
}

// API: Check if sensor is available
bool brightness_control_sensor_available(void) {
    return sensor_available;
}

// API: Read light sensor (main thread only!)
int brightness_control_read_sensor(uint16_t *light_val) {
    return apds9960_read_light(light_val);
}

// API: Map light value to brightness percentage
uint8_t brightness_control_map_light_to_brightness(uint32_t light_value) {
    uint8_t min_brightness = CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS;
    uint8_t max_brightness = CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS;
    uint32_t threshold = CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD;

    // Clamp to threshold
    if (light_value >= threshold) {
        return max_brightness;
    }

    // Non-linear mapping (square root curve for darker bias)
    uint32_t brightness_range = max_brightness - min_brightness;

    // Normalize light value to 0-1000 range for sqrt calculation
    uint32_t normalized = (light_value * 1000) / threshold;

    // Apply square root for non-linear curve (darker bias)
    uint32_t sqrt_val = 0;
    if (normalized > 0) {
        // Integer square root approximation
        uint32_t x = normalized;
        uint32_t y = (x + 1) / 2;
        while (y < x) {
            x = y;
            y = (x + normalized / x) / 2;
        }
        sqrt_val = x; // sqrt(0-1000) = 0-31
    }

    // Map sqrt result (0-31) to brightness range
    uint32_t scaled_brightness = (sqrt_val * brightness_range) / 32;

    return min_brightness + (uint8_t)scaled_brightness;
}

// Work Queue handler - ONLY sends periodic sensor read request
// CRITICAL: No I2C or PWM access here! Work Queue context!
// All sensor reading happens in main thread via message handler
static void brightness_sensor_work_handler(struct k_work *work) {
    if (!sensor_available) {
        goto reschedule;
    }

    // Send message to main thread to read sensor
    // Main thread will do I2C access safely
    scanner_msg_send_brightness_sensor_read();

reschedule:
    k_work_schedule(&brightness_sensor_work, K_MSEC(CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS));
}

// API: Enable/disable auto brightness
void brightness_control_set_auto(bool enabled) {
    auto_brightness_enabled = enabled;

    if (enabled && sensor_available) {
        // Trigger immediate sensor read
        k_work_schedule(&brightness_sensor_work, K_NO_WAIT);
        LOG_INF("🔆 Auto brightness enabled");
    } else {
        LOG_INF("🔆 Auto brightness disabled");
    }
}

// API: Check if auto is enabled
bool brightness_control_is_auto(void) {
    return auto_brightness_enabled;
}

static int brightness_control_init(void) {
    LOG_INF("🌞 Brightness Control: Message Queue Mode (Sensor)");

    // Get I2C device (from device tree)
    i2c_dev = NULL;
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c0), okay)
    i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
#endif

    if (!i2c_dev || !device_is_ready(i2c_dev)) {
        LOG_WRN("I2C device not ready - auto brightness disabled");
        sensor_available = false;
        return 0;
    }

    // Initialize APDS9960 for ALS operation
    int ret = apds9960_init_als();
    if (ret < 0) {
        LOG_WRN("APDS9960 init failed - auto brightness disabled");
        sensor_available = false;
        return 0;
    }

    sensor_available = true;

    LOG_INF("✅ Sensor brightness control ready (message queue mode)");
    LOG_INF("📊 Settings: Min=%u%%, Max=%u%%, Threshold=%u, Interval=%ums",
            CONFIG_PROSPECTOR_ALS_MIN_BRIGHTNESS,
            CONFIG_PROSPECTOR_ALS_MAX_BRIGHTNESS,
            CONFIG_PROSPECTOR_ALS_SENSOR_THRESHOLD,
            CONFIG_PROSPECTOR_ALS_UPDATE_INTERVAL_MS);

    // Initialize work queue
    k_work_init_delayable(&brightness_sensor_work, brightness_sensor_work_handler);

    // Start sensor monitoring after 1 second
    k_work_schedule(&brightness_sensor_work, K_MSEC(1000));

    return 0;
}

SYS_INIT(brightness_control_init, APPLICATION, 90);

#else  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n

// Fixed brightness mode - no sensor, no work queues
// All brightness control happens in scanner_display.c via messages

void brightness_control_set_auto(bool enabled) {
    ARG_UNUSED(enabled);
    LOG_WRN("🔆 Auto brightness not available (no sensor)");
}

bool brightness_control_is_auto(void) {
    return false;
}

const struct device *brightness_control_get_i2c_dev(void) {
    return NULL;
}

bool brightness_control_sensor_available(void) {
    return false;
}

int brightness_control_read_sensor(uint16_t *light_val) {
    ARG_UNUSED(light_val);
    return -ENODEV;
}

uint8_t brightness_control_map_light_to_brightness(uint32_t light_value) {
    ARG_UNUSED(light_value);
    return 50;  // Return mid brightness when no sensor
}

static int brightness_control_init(void) {
    LOG_INF("🔆 Brightness Control: Fixed Mode (no sensor)");
    return 0;
}

SYS_INIT(brightness_control_init, APPLICATION, 90);

#endif  // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR
