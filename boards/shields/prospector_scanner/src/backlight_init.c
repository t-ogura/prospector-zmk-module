/*
 * Simple backlight initialization - turns on backlight at boot
 * Uses PWM LED driver for brightness control
 *
 * This runs at SYS_INIT priority 50, BEFORE display initialization,
 * ensuring the backlight is on even if display init fails.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(backlight_init, LOG_LEVEL_INF);

/* Backlight PWM LED node */
#if DT_HAS_COMPAT_STATUS_OKAY(pwm_leds)
#define BACKLIGHT_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(pwm_leds)
static const struct device *backlight_dev = DEVICE_DT_GET(BACKLIGHT_NODE);
#define HAS_PWM_BACKLIGHT 1
#else
#define HAS_PWM_BACKLIGHT 0
#endif

static uint32_t heartbeat_count = 0;

/* Default brightness (0-100) */
#define DEFAULT_BRIGHTNESS 65

/* Heartbeat timer callback - for debugging */
static void heartbeat_timer_cb(struct k_timer *timer) {
    heartbeat_count++;
    LOG_INF("Heartbeat #%u - device alive", heartbeat_count);

    /* Check display status every 5 heartbeats */
    if (heartbeat_count % 5 == 0) {
        const struct device *display = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_display));
        if (display && device_is_ready(display)) {
            struct display_capabilities caps;
            display_get_capabilities(display, &caps);
            LOG_INF("Display: %ux%u, format=%d", caps.x_resolution, caps.y_resolution, caps.current_pixel_format);
        } else {
            LOG_WRN("Display not ready");
        }
    }
}

K_TIMER_DEFINE(heartbeat_timer, heartbeat_timer_cb, NULL);

#if HAS_PWM_BACKLIGHT

static int backlight_init(void) {
    int ret;

    LOG_INF("=== BACKLIGHT INIT (PWM, priority 50) ===");

    if (!device_is_ready(backlight_dev)) {
        LOG_ERR("PWM backlight device not ready");
        return -ENODEV;
    }

    /* Set initial brightness to default value */
    ret = led_set_brightness(backlight_dev, 0, DEFAULT_BRIGHTNESS);
    if (ret < 0) {
        LOG_ERR("Failed to set backlight brightness: %d", ret);
        return ret;
    }

    LOG_INF("Backlight turned ON at %d%% brightness", DEFAULT_BRIGHTNESS);

    /* Start heartbeat timer - every 3 seconds */
    k_timer_start(&heartbeat_timer, K_SECONDS(3), K_SECONDS(3));
    LOG_INF("Heartbeat timer started (3s interval)");

    return 0;
}

SYS_INIT(backlight_init, APPLICATION, 50);

#else
#warning "PWM backlight LED node not found"
#endif
