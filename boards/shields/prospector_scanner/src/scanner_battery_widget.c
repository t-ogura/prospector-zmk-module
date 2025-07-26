#include "scanner_battery_widget.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_PROSPECTOR_MODE_SCANNER) && IS_ENABLED(CONFIG_ZMK_DISPLAY)

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

// Battery bar widget structure mirrors the original design
static void set_battery_bar_value(lv_obj_t *container, uint8_t level, bool connected) {
    if (!container) return;
    
    lv_obj_t *bar = lv_obj_get_child(container, 0);
    lv_obj_t *num = lv_obj_get_child(container, 1);
    lv_obj_t *nc_bar = lv_obj_get_child(container, 2);
    lv_obj_t *nc_num = lv_obj_get_child(container, 3);
    
    if (!bar || !num || !nc_bar || !nc_num) return;
    
    if (connected) {
        // Show battery bar and percentage
        lv_obj_fade_out(nc_bar, 150, 0);
        lv_obj_fade_out(nc_num, 150, 0);
        lv_obj_fade_in(bar, 150, 250);
        lv_obj_fade_in(num, 150, 250);
        
        // Update battery level
        lv_bar_set_value(bar, level, LV_ANIM_ON);
        lv_label_set_text_fmt(num, "%d", level);
        
        // Set colors based on battery level
        if (level < 20) {
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xD3900F), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xE8AC11), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x6E4E07), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFFB802), 0);
        } else {
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x909090), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x202020), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFFFFFF), 0);
        }
    } else {
        // Show disconnected state
        lv_obj_fade_out(bar, 150, 0);
        lv_obj_fade_out(num, 150, 0);
        lv_obj_fade_in(nc_bar, 150, 250);
        lv_obj_fade_in(nc_num, 150, 250);
    }
}

static lv_obj_t *create_battery_container(lv_obj_t *parent) {
    lv_obj_t *info_container = lv_obj_create(parent);
    lv_obj_center(info_container);
    lv_obj_set_height(info_container, lv_pct(100));
    lv_obj_set_flex_grow(info_container, 1);
    
    // Battery bar (connected state)
    lv_obj_t *bar = lv_bar_create(info_container);
    lv_obj_set_size(bar, lv_pct(100), 4);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x909090), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xf0f0f0), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(bar, 250, 0);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_opa(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(bar, 255, LV_PART_INDICATOR);
    
    // Battery percentage label (connected state)
    lv_obj_t *num = lv_label_create(info_container);
    lv_obj_set_style_text_font(num, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(num, lv_color_white(), 0);
    lv_obj_set_style_opa(num, 255, 0);
    lv_obj_align(num, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(num, "N/A");
    lv_obj_set_style_opa(num, 0, 0);
    
    // Disconnected bar (not connected state)
    lv_obj_t *nc_bar = lv_obj_create(info_container);
    lv_obj_set_size(nc_bar, lv_pct(100), 4);
    lv_obj_align(nc_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nc_bar, lv_color_hex(0x9e2121), LV_PART_MAIN);
    lv_obj_set_style_radius(nc_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nc_bar, 255, 0);
    
    // Disconnected symbol (not connected state)
    lv_obj_t *nc_num = lv_label_create(info_container);
    lv_obj_set_style_text_color(nc_num, lv_color_hex(0xe63030), 0);
    lv_obj_align(nc_num, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(nc_num, LV_SYMBOL_CLOSE);
    lv_obj_set_style_opa(nc_num, 255, 0);
    
    return info_container;
}

int zmk_widget_scanner_battery_init(struct zmk_widget_scanner_battery *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_width(widget->obj, lv_pct(100));
    lv_obj_set_flex_flow(widget->obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(widget->obj,
                          LV_FLEX_ALIGN_SPACE_EVENLY, // Main axis (horizontal)
                          LV_FLEX_ALIGN_CENTER,       // Cross axis (vertical)
                          LV_FLEX_ALIGN_CENTER        // Track alignment
    );
    lv_obj_set_style_pad_column(widget->obj, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(widget->obj, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(widget->obj, 16, LV_PART_MAIN);
    
    // Create containers for Central and Peripheral devices
    // For now, create 2 containers (Central + 1 Peripheral)
    // This can be expanded later for more peripherals
    for (int i = 0; i < 2; i++) {
        create_battery_container(widget->obj);
    }
    
    sys_slist_append(&widgets, &widget->node);
    
    LOG_INF("Scanner battery widget initialized");
    return 0;
}

void zmk_widget_scanner_battery_update(struct zmk_widget_scanner_battery *widget, 
                                       struct zmk_keyboard_status *status) {
    if (!widget || !widget->obj || !status) {
        return;
    }
    
    LOG_INF("Updating scanner battery widget - device_role: %d, battery: %d%%", 
            status->data.device_role, status->data.battery_level);
    
    // Determine which container to update based on device role
    int container_index = -1;
    
    if (status->data.device_role == ZMK_DEVICE_ROLE_CENTRAL || 
        status->data.device_role == ZMK_DEVICE_ROLE_STANDALONE) {
        container_index = 0; // First container for Central/Standalone
    } else if (status->data.device_role == ZMK_DEVICE_ROLE_PERIPHERAL) {
        container_index = 1; // Second container for Peripheral
    }
    
    if (container_index >= 0 && container_index < 2) {
        lv_obj_t *container = lv_obj_get_child(widget->obj, container_index);
        if (container) {
            set_battery_bar_value(container, status->data.battery_level, true);
            LOG_INF("Updated battery container %d with %d%%", container_index, status->data.battery_level);
        }
    }
}

lv_obj_t *zmk_widget_scanner_battery_obj(struct zmk_widget_scanner_battery *widget) {
    return widget ? widget->obj : NULL;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY