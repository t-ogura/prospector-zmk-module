/*
 * Prospector V2 - Minimal Custom Status Screen
 *
 * Phase 1: Just labels on screen (no widgets, no containers)
 * Based on working display_test pattern
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/display/status_screen.h>

LOG_MODULE_REGISTER(prospector_v2, LOG_LEVEL_INF);

/* Font declarations */
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_unscii_16);

/* Main screen creation - LVGL 9 compatible */
lv_obj_t *zmk_display_status_screen(void) {
    LOG_INF("=============================================");
    LOG_INF("=== Prospector V2 - Phase 1 ===");
    LOG_INF("=== Labels only (no widgets) ===");
    LOG_INF("=============================================");

    /* Step 1: Create main screen */
    LOG_INF("[INIT] Creating main_screen...");
    lv_obj_t *screen = lv_obj_create(NULL);
    if (!screen) {
        LOG_ERR("Failed to create screen!");
        return NULL;
    }
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    LOG_INF("[INIT] main_screen created OK");

    /* Step 2: Title label - top center */
    LOG_INF("[INIT] Creating title label...");
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Prospector V2");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    LOG_INF("[INIT] title created OK");

    /* Step 3: Status label - center */
    LOG_INF("[INIT] Creating status label...");
    lv_obj_t *status = lv_label_create(screen);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(status, lv_color_make(0x00, 0xFF, 0x00), 0);
    lv_label_set_text(status, "Phase 1: OK");
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 0);
    LOG_INF("[INIT] status created OK");

    /* Step 4: Info label - bottom */
    LOG_INF("[INIT] Creating info label...");
    lv_obj_t *info = lv_label_create(screen);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(info, lv_color_make(0x80, 0x80, 0x80), 0);
    lv_label_set_text(info, "Labels Only");
    lv_obj_align(info, LV_ALIGN_BOTTOM_MID, 0, -20);
    LOG_INF("[INIT] info created OK");

    LOG_INF("=============================================");
    LOG_INF("=== Phase 1 Complete ===");
    LOG_INF("=============================================");

    return screen;
}
