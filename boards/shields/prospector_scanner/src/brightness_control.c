/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct device *pwm_dev;

// PWM configuration for backlight control (Pin 6 = P0.04)
#define PWM_PERIOD_USEC 1000U  // 1ms period = 1kHz
#define PWM_FLAGS 0

static void set_brightness_pwm(uint8_t brightness_percent) {
    if (!pwm_dev || !device_is_ready(pwm_dev)) {
        LOG_ERR("PWM device not ready for brightness control");
        return;
    }
    
    // Convert percentage to PWM duty cycle
    uint32_t pulse_width_nsec = (PWM_PERIOD_USEC * 1000 * brightness_percent) / 100;
    uint32_t period_nsec = PWM_PERIOD_USEC * 1000;
    
    int ret = pwm_set(pwm_dev, 0, period_nsec, pulse_width_nsec, PWM_FLAGS);
    
    if (ret < 0) {
        LOG_ERR("Failed to set PWM brightness: %d", ret);
    } else {
        LOG_DBG("Backlight brightness set to %d%% (PWM: %d/%d nsec)", 
                brightness_percent, pulse_width_nsec, period_nsec);
    }
}

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
    
    // Apply brightness via PWM
    set_brightness_pwm(brightness);
    LOG_INF("ALS: %d lux, Brightness set to: %d%%", light_level, brightness);
}

static void brightness_work_handler(struct k_work *work) {
    update_brightness();
    
    // Schedule next update
    k_work_schedule(&brightness_work, K_SECONDS(2));
}

static int brightness_control_init(void) {
    // Initialize PWM device
    pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm0));
    if (!device_is_ready(pwm_dev)) {
        LOG_ERR("PWM device not ready");
        return -ENODEV;
    }
    
    // Initialize ALS device
    als_dev = DEVICE_DT_GET(DT_ALIAS(als));
    if (!device_is_ready(als_dev)) {
        LOG_WRN("ALS device not ready, using fixed brightness");
        set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
        return 0;
    }
    
    LOG_INF("ALS device ready, starting automatic brightness control");
    
    // Initialize work queue
    k_work_init_delayable(&brightness_work, brightness_work_handler);
    
    // Start brightness monitoring
    k_work_schedule(&brightness_work, K_SECONDS(1));
    
    return 0;
}

#else // !CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

static int brightness_control_init(void) {
    // Initialize PWM device
    pwm_dev = DEVICE_DT_GET(DT_NODELABEL(pwm0));
    if (!device_is_ready(pwm_dev)) {
        LOG_ERR("PWM device not ready");
        return -ENODEV;
    }
    
    // Set fixed brightness
    set_brightness_pwm(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    LOG_INF("Brightness control initialized with fixed brightness: %d%%", 
            CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    return 0;
}

#endif // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

SYS_INIT(brightness_control_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);