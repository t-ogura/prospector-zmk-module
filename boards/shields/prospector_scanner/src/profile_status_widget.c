/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>

#include "profile_status_widget.h"

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct profile_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool usb_is_hid_ready;
};

static struct profile_status_state get_state(const zmk_event_t *_eh) {
    return (struct profile_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
        .usb_is_hid_ready = zmk_usb_is_hid_ready()
    };
}

static void set_profile_display(struct zmk_widget_profile_status *widget, struct profile_status_state state) {
    char profile_text[32];
    
    // Display active profile number (0-4)
    snprintf(profile_text, sizeof(profile_text), "Profile: %d", state.active_profile_index);
    lv_label_set_text(widget->profile_label, profile_text);
    
    // Set connection status color
    if (state.active_profile_connected) {
        lv_obj_set_style_text_color(widget->profile_label, lv_color_hex(0x00FF00), 0); // Green = connected
    } else if (state.active_profile_bonded) {
        lv_obj_set_style_text_color(widget->profile_label, lv_color_hex(0x0000FF), 0); // Blue = bonded but not connected
    } else {
        lv_obj_set_style_text_color(widget->profile_label, lv_color_hex(0xFFFFFF), 0); // White = not bonded
    }
    
    LOG_INF("Profile widget: index=%d, connected=%d, bonded=%d", 
            state.active_profile_index, state.active_profile_connected, state.active_profile_bonded);
}

static void profile_status_update_cb(struct profile_status_state state) {
    struct zmk_widget_profile_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_profile_display(widget, state);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_profile_status, struct profile_status_state,
                            profile_status_update_cb, get_state)
ZMK_SUBSCRIPTION(widget_profile_status, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(widget_profile_status, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(widget_profile_status, zmk_usb_conn_state_changed);

int zmk_widget_profile_status_init(struct zmk_widget_profile_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 180, 40);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);
    
    widget->profile_label = lv_label_create(widget->obj);
    lv_obj_align(widget->profile_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(widget->profile_label, "Profile: 0");
    lv_obj_set_style_text_color(widget->profile_label, lv_color_white(), 0);
    
    sys_slist_append(&widgets, &widget->node);
    widget_profile_status_init();
    
    LOG_INF("Profile status widget initialized");
    return 0;
}

lv_obj_t *zmk_widget_profile_status_obj(struct zmk_widget_profile_status *widget) {
    return widget ? widget->obj : NULL;
}