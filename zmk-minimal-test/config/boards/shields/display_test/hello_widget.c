/*
 * Simple Hello Widget - Zephyr 4.1 / LVGL 9 Test
 *
 * This widget demonstrates the Prospector widget pattern:
 * - Dynamic memory allocation with lv_mem_alloc/lv_mem_free
 * - Container object with child labels
 * - Create/Destroy lifecycle
 */

#include <lvgl.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <string.h>
#include "hello_widget.h"

LOG_MODULE_REGISTER(hello_widget, LOG_LEVEL_INF);

/* Declare fonts */
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_24);

/**
 * @brief Initialize Hello widget (internal function)
 */
static int zmk_widget_hello_init(struct zmk_widget_hello *widget, lv_obj_t *parent) {
    if (!widget || !parent) {
        return -1;
    }

    LOG_INF("Initializing Hello widget...");

    /* Create container widget */
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 200, 80);
    lv_obj_set_style_bg_color(widget->obj, lv_color_hex(0x1a1a2e), 0);  /* Dark blue background */
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(widget->obj, 2, 0);
    lv_obj_set_style_border_color(widget->obj, lv_color_hex(0x4a90d9), 0);  /* Light blue border */
    lv_obj_set_style_radius(widget->obj, 10, 0);  /* Rounded corners */
    lv_obj_set_style_pad_all(widget->obj, 5, 0);

    /* Create title label "HELLO" (smaller font) */
    widget->title_label = lv_label_create(widget->obj);
    lv_obj_align(widget->title_label, LV_ALIGN_TOP_MID, 0, 5);
    lv_label_set_text(widget->title_label, "HELLO");
    lv_obj_set_style_text_font(widget->title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(widget->title_label, lv_color_hex(0x88aaff), 0);  /* Light blue text */

    /* Create name label (larger font) */
    widget->name_label = lv_label_create(widget->obj);
    lv_obj_align(widget->name_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(widget->name_label, "Zephyr 4.1");
    lv_obj_set_style_text_font(widget->name_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(widget->name_label, lv_color_white(), 0);

    LOG_INF("Hello widget initialized successfully");
    return 0;
}

struct zmk_widget_hello *zmk_widget_hello_create(lv_obj_t *parent) {
    LOG_INF("Creating Hello widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    /* Allocate memory for widget structure using LVGL's memory allocator */
    struct zmk_widget_hello *widget =
        (struct zmk_widget_hello *)lv_malloc(sizeof(struct zmk_widget_hello));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for hello_widget (%d bytes)",
                sizeof(struct zmk_widget_hello));
        return NULL;
    }

    /* Zero-initialize the allocated memory */
    memset(widget, 0, sizeof(struct zmk_widget_hello));

    /* Initialize widget (this creates LVGL objects) */
    int ret = zmk_widget_hello_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_free(widget);
        return NULL;
    }

    LOG_INF("Hello widget created successfully at %p", (void *)widget);
    return widget;
}

void zmk_widget_hello_destroy(struct zmk_widget_hello *widget) {
    LOG_INF("Destroying Hello widget (dynamic deallocation)");

    if (!widget) {
        return;
    }

    /* Delete LVGL objects first (reverse order of creation) */
    if (widget->name_label) {
        lv_obj_del(widget->name_label);
        widget->name_label = NULL;
    }
    if (widget->title_label) {
        lv_obj_del(widget->title_label);
        widget->title_label = NULL;
    }
    if (widget->obj) {
        lv_obj_del(widget->obj);
        widget->obj = NULL;
    }

    /* Free the widget structure memory from LVGL heap */
    lv_free(widget);
    LOG_INF("Hello widget destroyed");
}

void zmk_widget_hello_set_name(struct zmk_widget_hello *widget, const char *name) {
    if (!widget || !widget->name_label || !name) {
        return;
    }

    lv_label_set_text(widget->name_label, name);
    LOG_INF("Hello widget name updated: %s", name);
}

lv_obj_t *zmk_widget_hello_obj(struct zmk_widget_hello *widget) {
    return widget ? widget->obj : NULL;
}
