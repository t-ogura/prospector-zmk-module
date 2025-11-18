#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zmk/status_scanner.h>

struct zmk_widget_scanner_battery {
    sys_snode_t node;
    lv_obj_t *obj;
    // Central/Peripheral labels removed for cleaner display
};

// ========== Dynamic Allocation Functions ==========

/**
 * @brief Create scanner battery widget with dynamic memory allocation
 *
 * @param parent Parent LVGL object
 * @return Pointer to allocated widget or NULL on failure
 */
struct zmk_widget_scanner_battery *zmk_widget_scanner_battery_create(lv_obj_t *parent);

/**
 * @brief Destroy scanner battery widget and free memory
 *
 * @param widget Widget to destroy
 */
void zmk_widget_scanner_battery_destroy(struct zmk_widget_scanner_battery *widget);

// ========== Widget Control Functions ==========

int zmk_widget_scanner_battery_init(struct zmk_widget_scanner_battery *widget, lv_obj_t *parent);
void zmk_widget_scanner_battery_update(struct zmk_widget_scanner_battery *widget,
                                       struct zmk_keyboard_status *status);
void zmk_widget_scanner_battery_reset(struct zmk_widget_scanner_battery *widget);
lv_obj_t *zmk_widget_scanner_battery_obj(struct zmk_widget_scanner_battery *widget);