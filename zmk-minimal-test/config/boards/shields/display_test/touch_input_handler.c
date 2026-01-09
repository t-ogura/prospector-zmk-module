/**
 * Touch Input Handler - Swipe detection and event raising
 *
 * NOTE: CST816S driver sends BTN_TOUCH=1 on EVERY position update,
 * not just on initial touch. We must track state transitions to detect
 * the actual first touch event.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include "swipe_event.h"

LOG_MODULE_REGISTER(touch_handler, LOG_LEVEL_DBG);

/* Swipe detection parameters */
#define SWIPE_THRESHOLD_PX  50   /* Minimum distance for swipe */
#define TAP_THRESHOLD_PX    20   /* Maximum distance for tap */

/* Touch state */
static int16_t touch_start_x = 0;
static int16_t touch_start_y = 0;
static int16_t touch_current_x = 0;
static int16_t touch_current_y = 0;
static bool touch_active = false;
static bool prev_touch_active = false;  /* Track previous state for transition detection */
static uint32_t touch_start_time = 0;

/* Detect and raise swipe event */
static void detect_swipe(void)
{
    int16_t raw_dx = touch_current_x - touch_start_x;
    int16_t raw_dy = touch_current_y - touch_start_y;

    /*
     * COORDINATE TRANSFORM for 90° rotated display:
     * - Physical vertical swipe (UP/DOWN) → touch panel X axis changes
     * - Physical horizontal swipe (LEFT/RIGHT) → touch panel Y axis changes
     *
     * Transform: swap axes and invert for correct logical direction
     */
    int16_t dx = -raw_dy;  /* Physical horizontal = raw Y (inverted) */
    int16_t dy = -raw_dx;  /* Physical vertical = raw X (inverted) */

    int16_t abs_dx = dx < 0 ? -dx : dx;
    int16_t abs_dy = dy < 0 ? -dy : dy;

    LOG_DBG("Swipe raw: dx=%d dy=%d → physical: dx=%d dy=%d", raw_dx, raw_dy, dx, dy);

    enum swipe_direction direction = SWIPE_DIRECTION_NONE;

    /* Determine swipe direction */
    if (abs_dx > SWIPE_THRESHOLD_PX || abs_dy > SWIPE_THRESHOLD_PX) {
        if (abs_dx > abs_dy) {
            /* Horizontal swipe */
            direction = (dx > 0) ? SWIPE_DIRECTION_RIGHT : SWIPE_DIRECTION_LEFT;
        } else {
            /* Vertical swipe */
            direction = (dy > 0) ? SWIPE_DIRECTION_DOWN : SWIPE_DIRECTION_UP;
        }
    }

    if (direction != SWIPE_DIRECTION_NONE) {
        const char *dir_name =
            direction == SWIPE_DIRECTION_UP ? "UP" :
            direction == SWIPE_DIRECTION_DOWN ? "DOWN" :
            direction == SWIPE_DIRECTION_LEFT ? "LEFT" : "RIGHT";

        LOG_INF("Swipe detected: %s (dx=%d, dy=%d)", dir_name, dx, dy);

        /* Raise ZMK event - thread-safe */
        raise_zmk_swipe_gesture_event(
            (struct zmk_swipe_gesture_event){.direction = direction}
        );
    } else if (abs_dx < TAP_THRESHOLD_PX && abs_dy < TAP_THRESHOLD_PX) {
        LOG_INF("Tap at x=%d, y=%d", touch_current_x, touch_current_y);
    }
}

/* Input callback - receives all touch events */
static void touch_input_callback(struct input_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    /* Debug: log all input events */
    LOG_DBG("Input: code=%d value=%d sync=%d", evt->code, evt->value, evt->sync);

    switch (evt->code) {
    case INPUT_ABS_X:
        touch_current_x = evt->value;
        break;

    case INPUT_ABS_Y:
        touch_current_y = evt->value;
        break;

    case INPUT_BTN_TOUCH:
        touch_active = (evt->value != 0);

        /* Detect touch start (false → true transition) */
        bool touch_started = touch_active && !prev_touch_active;

        if (touch_started) {
            /* Touch down - record start position ONLY on first touch */
            touch_start_time = k_uptime_get_32();
            touch_start_x = touch_current_x;
            touch_start_y = touch_current_y;
            LOG_INF("Touch DOWN: start=(%d,%d)", touch_start_x, touch_start_y);
        } else if (!touch_active && prev_touch_active) {
            /* Touch up (true → false transition) - detect gesture */
            LOG_INF("Touch UP: end=(%d,%d) start=(%d,%d)",
                    touch_current_x, touch_current_y, touch_start_x, touch_start_y);
            detect_swipe();
        }

        /* Update previous state for next event */
        prev_touch_active = touch_active;
        break;

    default:
        LOG_DBG("Unknown input code: %d", evt->code);
        break;
    }
}

/* Register input callback for ALL input devices */
INPUT_CALLBACK_DEFINE(NULL, touch_input_callback, NULL);

static int touch_handler_init(void)
{
    LOG_INF("Touch handler initialized (swipe threshold: %d px)", SWIPE_THRESHOLD_PX);
    return 0;
}

SYS_INIT(touch_handler_init, APPLICATION, 95);
