#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zmk/status_scanner.h>

struct zmk_widget_scanner_battery {
    sys_snode_t node;
    lv_obj_t *obj;
};

int zmk_widget_scanner_battery_init(struct zmk_widget_scanner_battery *widget, lv_obj_t *parent);
void zmk_widget_scanner_battery_update(struct zmk_widget_scanner_battery *widget, 
                                       struct zmk_keyboard_status *status);
lv_obj_t *zmk_widget_scanner_battery_obj(struct zmk_widget_scanner_battery *widget);