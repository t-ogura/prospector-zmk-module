/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/device.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER)

// PWM device for backlight control
static const struct pwm_dt_spec backlight_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(backlight));
static uint8_t current_brightness = 100;

int zmk_display_set_brightness(uint8_t brightness) {
    if (brightness > 100) {
        brightness = 100;
    }
    
    if (!device_is_ready(backlight_pwm.dev)) {
        LOG_ERR("Backlight PWM device not ready");
        return -ENODEV;
    }
    
    // Convert percentage to PWM duty cycle
    uint32_t pulse_width = (backlight_pwm.period * brightness) / 100;
    
    int ret = pwm_set_pulse_dt(&backlight_pwm, pulse_width);
    if (ret < 0) {
        LOG_ERR("Failed to set backlight brightness: %d", ret);
        return ret;
    }
    
    current_brightness = brightness;
    LOG_DBG("Display brightness set to %d%%", brightness);
    
    return 0;
}

uint8_t zmk_display_get_brightness(void) {
    return current_brightness;
}

// Initialize with default brightness
static int display_brightness_init(void) {
    LOG_INF("Initializing display brightness control");
    
    if (!device_is_ready(backlight_pwm.dev)) {
        LOG_ERR("Backlight PWM device not ready");
        return -ENODEV;
    }
    
    // Set initial brightness to 100%
    return zmk_display_set_brightness(100);
}

SYS_INIT(display_brightness_init, APPLICATION, 70);

#endif // CONFIG_PROSPECTOR_MODE_SCANNER