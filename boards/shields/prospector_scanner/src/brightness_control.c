/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
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

static void __attribute__((unused)) brightness_work_handler(struct k_work *work) {
    update_brightness();
    
    // Schedule next update
    k_work_schedule(&brightness_work, K_SECONDS(2));
}

static int brightness_control_init(void) {
    LOG_WRN("ALS device not configured, using fixed brightness");
    // TODO: Implement fixed brightness setting
    return 0;
}

#else // !CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

static int brightness_control_init(void) {
    // TODO: Implement fixed brightness setting
    LOG_INF("Brightness control initialized with fixed brightness: %d%% (not implemented)", 
            CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);
    return 0;
}

#endif // CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR

SYS_INIT(brightness_control_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);