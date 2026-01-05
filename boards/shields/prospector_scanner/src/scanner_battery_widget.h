#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zmk/status_scanner.h>

/**
 * LVGL 9 FIX: NO CONTAINER pattern
 * All elements created directly on parent screen to avoid freeze bug.
 * Each battery slot has: bar + percentage label + disconnected bar + disconnected symbol
 */
#define SCANNER_BATTERY_SLOTS 2  // Central + Peripheral

struct zmk_widget_scanner_battery {
    sys_snode_t node;
    lv_obj_t *obj;  // Points to first bar for compatibility
    lv_obj_t *parent;  // Store parent for positioning

    // LVGL 9: Direct pointers to each element (no container)
    lv_obj_t *bar[SCANNER_BATTERY_SLOTS];      // Battery level bars
    lv_obj_t *num[SCANNER_BATTERY_SLOTS];      // Percentage labels
    lv_obj_t *nc_bar[SCANNER_BATTERY_SLOTS];   // Disconnected state bars
    lv_obj_t *nc_num[SCANNER_BATTERY_SLOTS];   // Disconnected symbols
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