/*
 * Simple Hello Widget - Zephyr 4.1 / LVGL 9 Test
 */

#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Simple Hello Widget structure
 */
struct zmk_widget_hello {
    lv_obj_t *obj;           // Container object
    lv_obj_t *title_label;   // "HELLO" label (small font)
    lv_obj_t *name_label;    // Name value (larger font)
};

/**
 * @brief Create Hello widget with dynamic memory allocation
 *
 * @param parent Parent LVGL object
 * @return Pointer to allocated widget or NULL on failure
 */
struct zmk_widget_hello *zmk_widget_hello_create(lv_obj_t *parent);

/**
 * @brief Destroy Hello widget and free memory
 *
 * @param widget Widget to destroy
 */
void zmk_widget_hello_destroy(struct zmk_widget_hello *widget);

/**
 * @brief Set the name text displayed in the widget
 *
 * @param widget Widget to update
 * @param name Text to display
 */
void zmk_widget_hello_set_name(struct zmk_widget_hello *widget, const char *name);

/**
 * @brief Get widget LVGL object
 *
 * @param widget Widget structure
 * @return LVGL object pointer or NULL
 */
lv_obj_t *zmk_widget_hello_obj(struct zmk_widget_hello *widget);

#ifdef __cplusplus
}
#endif
