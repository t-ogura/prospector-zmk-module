/**
 * LVGL 9 FIX: NO CONTAINER Pattern
 *
 * All elements created directly on parent screen to avoid LVGL 9 freeze bug.
 * Previously: Nested containers with flex layout caused MCU freeze.
 * Now: Direct element creation with absolute positioning.
 */

#include "scanner_battery_widget.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

// Position constants for NO CONTAINER layout
// Widget positioned at BOTTOM_MID with y=-20 in scanner_display.c
// These offsets are relative to BOTTOM_MID alignment
#define BAR_WIDTH       110
#define BAR_HEIGHT      4
#define BAR_Y_OFFSET    -8     // Distance from bottom
#define LABEL_Y_OFFSET  -25    // Label above bar
#define LEFT_X_OFFSET   -70    // Left battery x offset from center
#define RIGHT_X_OFFSET  70     // Right battery x offset from center

// Set battery bar value - LVGL 9 version using direct pointers
static void set_battery_bar_value(struct zmk_widget_scanner_battery *widget,
                                   int slot, uint8_t level, bool connected) {
    if (!widget || slot < 0 || slot >= SCANNER_BATTERY_SLOTS) return;

    lv_obj_t *bar = widget->bar[slot];
    lv_obj_t *num = widget->num[slot];
    lv_obj_t *nc_bar = widget->nc_bar[slot];
    lv_obj_t *nc_num = widget->nc_num[slot];

    if (!bar || !num || !nc_bar || !nc_num) return;

    if (connected) {
        // Show battery bar and percentage
        lv_obj_clear_flag(nc_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(nc_num, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(nc_bar, 0, 0);
        lv_obj_set_style_opa(nc_num, 0, 0);
        lv_obj_set_style_opa(bar, 255, LV_PART_MAIN);
        lv_obj_set_style_opa(bar, 255, LV_PART_INDICATOR);
        lv_obj_set_style_opa(num, 255, 0);

        // Update battery level
        lv_bar_set_value(bar, level, LV_ANIM_OFF);
        lv_label_set_text_fmt(num, "%d", level);

        // 5-level color-coded battery visualization
        if (level >= 80) {
            // Green
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x00CC66), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0x00FF66), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x003311), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0x00FF66), 0);
        } else if (level >= 60) {
            // Light Green
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x66CC00), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0x99FF33), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x223300), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0x99FF33), 0);
        } else if (level >= 40) {
            // Yellow
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFCC00), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xFFDD33), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x332200), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFFDD33), 0);
        } else if (level >= 20) {
            // Orange
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF8800), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xFF9933), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x331100), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFF9933), 0);
        } else {
            // Red (critical)
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF3333), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xFF6666), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x330000), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFF6666), 0);
        }
    } else {
        // Show disconnected state
        lv_obj_set_style_opa(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_opa(bar, 0, LV_PART_INDICATOR);
        lv_obj_set_style_opa(num, 0, 0);
        lv_obj_set_style_opa(nc_bar, 255, 0);
        lv_obj_set_style_opa(nc_num, 255, 0);
    }
}

/**
 * LVGL 9 FIX: NO CONTAINER - Create all elements directly on parent
 */
int zmk_widget_scanner_battery_init(struct zmk_widget_scanner_battery *widget, lv_obj_t *parent) {
    if (!widget || !parent) return -1;

    widget->parent = parent;

    // X offsets for left (Peripheral) and right (Central) batteries
    int x_offsets[SCANNER_BATTERY_SLOTS] = { LEFT_X_OFFSET, RIGHT_X_OFFSET };

    for (int i = 0; i < SCANNER_BATTERY_SLOTS; i++) {
        int x_off = x_offsets[i];

        // Battery bar (connected state) - created directly on parent
        widget->bar[i] = lv_bar_create(parent);
        lv_obj_set_size(widget->bar[i], BAR_WIDTH, BAR_HEIGHT);
        lv_obj_align(widget->bar[i], LV_ALIGN_BOTTOM_MID, x_off, BAR_Y_OFFSET);
        lv_bar_set_range(widget->bar[i], 0, 100);
        lv_bar_set_value(widget->bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(widget->bar[i], lv_color_hex(0x202020), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(widget->bar[i], 255, LV_PART_MAIN);
        lv_obj_set_style_radius(widget->bar[i], 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(widget->bar[i], lv_color_hex(0x909090), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(widget->bar[i], 255, LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(widget->bar[i], lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(widget->bar[i], LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
        lv_obj_set_style_radius(widget->bar[i], 1, LV_PART_INDICATOR);
        lv_obj_set_style_opa(widget->bar[i], 0, LV_PART_MAIN);  // Initially hidden
        lv_obj_set_style_opa(widget->bar[i], 0, LV_PART_INDICATOR);

        // Battery percentage label (connected state)
        widget->num[i] = lv_label_create(parent);
        lv_obj_set_style_text_font(widget->num[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(widget->num[i], lv_color_white(), 0);
        lv_obj_align(widget->num[i], LV_ALIGN_BOTTOM_MID, x_off, LABEL_Y_OFFSET);
        lv_label_set_text(widget->num[i], "N/A");
        lv_obj_set_style_opa(widget->num[i], 0, 0);  // Initially hidden

        // Disconnected bar
        widget->nc_bar[i] = lv_obj_create(parent);
        lv_obj_set_size(widget->nc_bar[i], BAR_WIDTH, BAR_HEIGHT);
        lv_obj_align(widget->nc_bar[i], LV_ALIGN_BOTTOM_MID, x_off, BAR_Y_OFFSET);
        lv_obj_set_style_bg_color(widget->nc_bar[i], lv_color_hex(0x9e2121), LV_PART_MAIN);
        lv_obj_set_style_radius(widget->nc_bar[i], 1, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(widget->nc_bar[i], 255, 0);
        lv_obj_set_style_border_width(widget->nc_bar[i], 0, 0);
        lv_obj_set_style_opa(widget->nc_bar[i], 255, 0);  // Initially visible (disconnected)

        // Disconnected symbol
        widget->nc_num[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(widget->nc_num[i], lv_color_hex(0xe63030), 0);
        lv_obj_align(widget->nc_num[i], LV_ALIGN_BOTTOM_MID, x_off, LABEL_Y_OFFSET);
        lv_label_set_text(widget->nc_num[i], LV_SYMBOL_CLOSE);
        lv_obj_set_style_opa(widget->nc_num[i], 255, 0);  // Initially visible (disconnected)
    }

    // Set widget->obj to first bar for compatibility with zmk_widget_scanner_battery_obj()
    widget->obj = widget->bar[0];

    sys_slist_append(&widgets, &widget->node);

    LOG_INF("✨ Scanner battery widget initialized (LVGL9 no-container pattern)");
    return 0;
}

// ========== Dynamic Allocation Functions ==========

struct zmk_widget_scanner_battery *zmk_widget_scanner_battery_create(lv_obj_t *parent) {
    LOG_DBG("Creating scanner battery widget (LVGL9 no-container)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    struct zmk_widget_scanner_battery *widget =
        (struct zmk_widget_scanner_battery *)lv_malloc(sizeof(struct zmk_widget_scanner_battery));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for scanner_battery_widget (%d bytes)",
                sizeof(struct zmk_widget_scanner_battery));
        return NULL;
    }

    memset(widget, 0, sizeof(struct zmk_widget_scanner_battery));

    int ret = zmk_widget_scanner_battery_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_free(widget);
        return NULL;
    }

    LOG_DBG("Scanner battery widget created successfully");
    return widget;
}

void zmk_widget_scanner_battery_destroy(struct zmk_widget_scanner_battery *widget) {
    LOG_DBG("Destroying scanner battery widget (LVGL9 no-container)");

    if (!widget) {
        return;
    }

    sys_slist_find_and_remove(&widgets, &widget->node);

    // LVGL 9 FIX: Delete each element individually (no container parent)
    for (int i = 0; i < SCANNER_BATTERY_SLOTS; i++) {
        if (widget->nc_num[i]) {
            lv_obj_del(widget->nc_num[i]);
            widget->nc_num[i] = NULL;
        }
        if (widget->nc_bar[i]) {
            lv_obj_del(widget->nc_bar[i]);
            widget->nc_bar[i] = NULL;
        }
        if (widget->num[i]) {
            lv_obj_del(widget->num[i]);
            widget->num[i] = NULL;
        }
        if (widget->bar[i]) {
            lv_obj_del(widget->bar[i]);
            widget->bar[i] = NULL;
        }
    }

    widget->obj = NULL;
    widget->parent = NULL;

    lv_free(widget);
}

void zmk_widget_scanner_battery_update(struct zmk_widget_scanner_battery *widget,
                                       struct zmk_keyboard_status *status) {
    if (!widget || !status) {
        return;
    }

    LOG_DBG("Battery widget update - Role:%d, Central:%d%%, Peripheral:[%d,%d,%d]",
            status->data.device_role, status->data.battery_level,
            status->data.peripheral_battery[0], status->data.peripheral_battery[1],
            status->data.peripheral_battery[2]);

    // Handle split keyboard display
    bool has_peripheral = (status->data.peripheral_battery[0] > 0 ||
                          status->data.peripheral_battery[1] > 0 ||
                          status->data.peripheral_battery[2] > 0);

    if (status->data.device_role == ZMK_DEVICE_ROLE_CENTRAL && has_peripheral) {
        // Split keyboard: show both batteries
        // Slot 0 (Left): Peripheral battery
        set_battery_bar_value(widget, 0, status->data.peripheral_battery[0], true);
        // Slot 1 (Right): Central battery
        set_battery_bar_value(widget, 1, status->data.battery_level, true);

        LOG_INF("✅ Split mode: Left=%d%%, Right=%d%%",
               status->data.peripheral_battery[0], status->data.battery_level);
    } else {
        // Single device or Central without connected peripherals
        LOG_INF("⚠️  Single mode: Central only %d%%", status->data.battery_level);

        // Slot 0: Central device
        set_battery_bar_value(widget, 0, status->data.battery_level, true);
        // Slot 1: Disconnected
        set_battery_bar_value(widget, 1, 0, false);
    }
}

void zmk_widget_scanner_battery_reset(struct zmk_widget_scanner_battery *widget) {
    if (!widget) {
        return;
    }

    LOG_INF("Battery widget reset - clearing all displays");

    for (int i = 0; i < SCANNER_BATTERY_SLOTS; i++) {
        set_battery_bar_value(widget, i, 0, false);
    }
}

lv_obj_t *zmk_widget_scanner_battery_obj(struct zmk_widget_scanner_battery *widget) {
    return widget ? widget->obj : NULL;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY
