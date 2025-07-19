/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR)

static const struct device *als_dev;
static struct k_work_delayable brightness_work;

static void update_brightness(void) {
    if (!als_dev) {
        return;
    }
    
    int ret = sensor_sample_fetch(als_dev);
    if (ret < 0) {
        LOG_WRN("Failed to fetch ALS sample: %d", ret);
        return;
    }
    
    struct sensor_value als_val;
    ret = sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &als_val);
    if (ret < 0) {
        LOG_WRN("Failed to get ALS value: %d", ret);
        return;
    }
    
    // Convert sensor value to brightness percentage
    int32_t light_level = als_val.val1;
    uint8_t brightness;
    
    if (light_level < 10) {
        brightness = 10;  // Minimum brightness
    } else if (light_level > 1000) {
        brightness = 100; // Maximum brightness
    } else {
        // Linear mapping from 10-1000 lux to 10-100% brightness
        brightness = 10 + ((light_level - 10) * 90) / 990;
    }
    
    // Set display brightness - TODO: Implement proper brightness control
    // Currently ZMK doesn't have a standard brightness API
    // This would need to be implemented in the display driver
    LOG_DBG("ALS: %d lux, Target brightness: %d%% (not implemented)", light_level, brightness);
}

static void brightness_work_handler(struct k_work *work) {
    update_brightness();
    
    // Schedule next update
    k_work_schedule(&brightness_work, K_SECONDS(2));
}

static int brightness_control_init(void) {
    als_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(als));
    if (!als_dev) {
        LOG_WRN("ALS device not found, using fixed brightness");
        return 0;
    }
    
    if (!device_is_ready(als_dev)) {
        LOG_WRN("ALS device not ready, using fixed brightness");
        als_dev = NULL;
        return 0;
    }
    
    k_work_init_delayable(&brightness_work, brightness_work_handler);
    k_work_schedule(&brightness_work, K_SECONDS(1));
    
    LOG_INF("ALS brightness control initialized");
    return 0;
}

#else // !CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

// PWM backlight control following original dongle design
static const struct device *backlight_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(backlight));

static int set_backlight(uint8_t brightness_percent) {
    if (!backlight_dev) {
        LOG_ERR("Backlight device not found");
        return -ENODEV;
    }
    
    if (!device_is_ready(backlight_dev)) {
        LOG_ERR("Backlight device not ready");
        return -ENODEV;
    }
    
    // Calculate PWM duty cycle (0-100% brightness)
    uint32_t period_usec = 1000; // 1kHz PWM
    uint32_t pulse_usec = (period_usec * brightness_percent) / 100;
    
    int ret = pwm_set_usec(backlight_dev, 0, period_usec, pulse_usec, 0);
    if (ret < 0) {
        LOG_ERR("Failed to set PWM backlight: %d", ret);
        return ret;
    }
    
    LOG_INF("Backlight set to %d%% via PWM (original dongle style)", brightness_percent);
    return 0;
}

static int brightness_control_init(void) {
    LOG_INF("Initializing PWM brightness control (original dongle style)");
    
    // Set fixed brightness
    int ret = set_backlight(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    if (ret < 0) {
        LOG_ERR("Failed to set backlight brightness: %d", ret);
        return ret;
    }
    
    LOG_INF("PWM brightness control initialized successfully");
    return 0;
}

#endif // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

SYS_INIT(brightness_control_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);