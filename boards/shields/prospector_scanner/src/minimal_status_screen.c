/*
 * Minimal status screen for boot testing
 * Just displays "Hello" - no complex widgets
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>

LOG_MODULE_REGISTER(minimal_screen, LOG_LEVEL_INF);

LV_FONT_DECLARE(lv_font_montserrat_20);

lv_obj_t *zmk_display_status_screen(void) {
    LOG_INF("=== MINIMAL STATUS SCREEN ===");

    /* Create main screen */
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    LOG_INF("Screen created");

    /* Create simple label */
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, "Hello Prospector!");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    LOG_INF("Label created");

    LOG_INF("=== MINIMAL SCREEN COMPLETE ===");
    return screen;
}
