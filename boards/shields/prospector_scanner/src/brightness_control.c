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

// Simple GPIO control following original dongle design
static const struct gpio_dt_spec backlight_gpio = GPIO_DT_SPEC_GET_OR(DT_PATH(backlight_gpio, backlight_gpio_pin), gpios, {0});

static int set_backlight(uint8_t brightness_percent) {
    if (!gpio_is_ready_dt(&backlight_gpio)) {
        LOG_ERR("GPIO backlight not ready");
        return -ENODEV;
    }
    
    // Simple on/off control - turn on if brightness > 0, following original dongle design
    bool enable = brightness_percent > 0;
    int ret = gpio_pin_set_dt(&backlight_gpio, enable ? 1 : 0);
    if (ret < 0) {
        LOG_ERR("Failed to set GPIO backlight: %d", ret);
        return ret;
    }
    
    LOG_INF("Backlight %s via GPIO (original dongle style)", enable ? "enabled" : "disabled");
    return 0;
}

static int brightness_control_init(void) {
    LOG_INF("Initializing GPIO brightness control (original dongle style)");
    
    // Configure GPIO output
    if (gpio_is_ready_dt(&backlight_gpio)) {
        int ret = gpio_pin_configure_dt(&backlight_gpio, GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure GPIO backlight: %d", ret);
            return ret;
        }
        LOG_INF("GPIO backlight configured successfully");
    }
    
    // Set fixed brightness (on)
    int ret = set_backlight(CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    if (ret < 0) {
        LOG_ERR("Failed to set backlight brightness: %d", ret);
        return ret;
    }
    
    LOG_INF("GPIO brightness control initialized successfully");
    return 0;
}

#endif // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

SYS_INIT(brightness_control_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);