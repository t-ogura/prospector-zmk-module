/*
 * Simple backlight initialization - turns on backlight at boot
 * Based on zmk-minimal-test/backlight_init.c
 *
 * This runs at SYS_INIT priority 50, BEFORE display initialization,
 * ensuring the backlight is on even if display init fails.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(backlight_init, LOG_LEVEL_INF);

/* Backlight GPIO: P1.11 (via led0 alias) */
#define BACKLIGHT_NODE DT_ALIAS(led0)

static uint32_t heartbeat_count = 0;

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

#if DT_NODE_EXISTS(BACKLIGHT_NODE)
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(BACKLIGHT_NODE, gpios);

static int backlight_init(void) {
    int ret;

    LOG_INF("=== BACKLIGHT INIT (priority 50) ===");

    if (!gpio_is_ready_dt(&backlight)) {
        LOG_ERR("Backlight GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure backlight GPIO: %d", ret);
        return ret;
    }

    /* Turn on backlight */
    ret = gpio_pin_set_dt(&backlight, 1);
    if (ret < 0) {
        LOG_ERR("Failed to turn on backlight: %d", ret);
        return ret;
    }

    LOG_INF("Backlight turned ON");

    /* Start heartbeat timer - every 3 seconds */
    k_timer_start(&heartbeat_timer, K_SECONDS(3), K_SECONDS(3));
    LOG_INF("Heartbeat timer started (3s interval)");

    return 0;
}

SYS_INIT(backlight_init, APPLICATION, 50);
#else
#warning "Backlight LED node (led0 alias) not found"
#endif
