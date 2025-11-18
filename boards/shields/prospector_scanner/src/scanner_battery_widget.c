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
        // Show battery bar and percentage (no animation to prevent flickering)
        lv_obj_set_style_opa(nc_bar, 0, 0);
        lv_obj_set_style_opa(nc_num, 0, 0);
        lv_obj_set_style_opa(bar, 255, 0);
        lv_obj_set_style_opa(num, 255, 0);
        
        // Update battery level without animation
        lv_bar_set_value(bar, level, LV_ANIM_OFF);
        lv_label_set_text_fmt(num, "%d", level);
        
        // 5-level color-coded battery visualization (enhanced scheme)
        if (level >= 80) {
            // 80%+ Green
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x00CC66), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0x00FF66), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x003311), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0x00FF66), 0);
        } else if (level >= 60) {
            // 60-79% Light Green
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x66CC00), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0x99FF33), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x223300), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0x99FF33), 0);
        } else if (level >= 40) {
            // 40-59% Yellow
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFCC00), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xFFDD33), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x332200), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFFDD33), 0);
        } else if (level >= 20) {
            // 20-39% Orange (new level)
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF8800), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xFF9933), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x331100), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFF9933), 0);
        } else {
            // <20% Red (critical level)
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF3333), LV_PART_INDICATOR);
            lv_obj_set_style_bg_grad_color(bar, lv_color_hex(0xFF6666), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x330000), LV_PART_MAIN);
            lv_obj_set_style_text_color(num, lv_color_hex(0xFF6666), 0);
        }
    } else {
        // Show disconnected state (no animation)
        lv_obj_set_style_opa(bar, 0, 0);
        lv_obj_set_style_opa(num, 0, 0);
        lv_obj_set_style_opa(nc_bar, 255, 0);
        lv_obj_set_style_opa(nc_num, 255, 0);
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
    
    // Central/Peripheral labels removed - cleaner display without positioning issues
    
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

// ========== Dynamic Allocation Functions ==========

struct zmk_widget_scanner_battery *zmk_widget_scanner_battery_create(lv_obj_t *parent) {
    LOG_DBG("Creating scanner battery widget (dynamic allocation)");

    if (!parent) {
        LOG_ERR("Cannot create widget: parent is NULL");
        return NULL;
    }

    // Allocate memory for widget structure using LVGL's memory allocator
    struct zmk_widget_scanner_battery *widget =
        (struct zmk_widget_scanner_battery *)lv_mem_alloc(sizeof(struct zmk_widget_scanner_battery));
    if (!widget) {
        LOG_ERR("Failed to allocate memory for scanner_battery_widget (%d bytes)",
                sizeof(struct zmk_widget_scanner_battery));
        return NULL;
    }

    // Zero-initialize the allocated memory
    memset(widget, 0, sizeof(struct zmk_widget_scanner_battery));

    // Initialize widget (this creates LVGL objects)
    int ret = zmk_widget_scanner_battery_init(widget, parent);
    if (ret != 0) {
        LOG_ERR("Widget initialization failed, freeing memory");
        lv_mem_free(widget);
        return NULL;
    }

    LOG_DBG("Scanner battery widget created successfully");
    return widget;
}

void zmk_widget_scanner_battery_destroy(struct zmk_widget_scanner_battery *widget) {
    LOG_DBG("Destroying scanner battery widget (dynamic deallocation)");

    if (!widget) {
        return;
    }

    // Remove from widgets list if it was added
    sys_slist_find_and_remove(&widgets, &widget->node);

    // Delete LVGL objects (parent obj will delete all children)
    if (widget->obj) {
        lv_obj_del(widget->obj);
        widget->obj = NULL;
    }

    // Free the widget structure memory from LVGL heap
    lv_mem_free(widget);
}

void zmk_widget_scanner_battery_update(struct zmk_widget_scanner_battery *widget, 
                                       struct zmk_keyboard_status *status) {
    if (!widget || !widget->obj || !status) {
        return;
    }
    
    LOG_DBG("Battery widget update - Role:%d, Central:%d%%, Peripheral:[%d,%d,%d]", 
            status->data.device_role, status->data.battery_level,
            status->data.peripheral_battery[0], status->data.peripheral_battery[1], 
            status->data.peripheral_battery[2]);
    
    // Handle split keyboard display - check if any peripheral is connected
    bool has_peripheral = (status->data.peripheral_battery[0] > 0 || 
                          status->data.peripheral_battery[1] > 0 || 
                          status->data.peripheral_battery[2] > 0);
    
    if (status->data.device_role == ZMK_DEVICE_ROLE_CENTRAL && has_peripheral) {
        // Split keyboard: show both Central and Peripheral batteries
        
        // Container 0: Peripheral (Left side) - Left display for Left keyboard
        lv_obj_t *peripheral_container = lv_obj_get_child(widget->obj, 0);
        if (peripheral_container) {
            set_battery_bar_value(peripheral_container, status->data.peripheral_battery[0], true);
            // peripheral_battery[0] is always LEFT keyboard according to protocol
            LOG_INF("✅ Split mode: Left=%d%%, Right=%d%%", 
               status->data.peripheral_battery[0], status->data.battery_level);
        }
        
        // Container 1: Central (Right side) - Right display for Right keyboard  
        lv_obj_t *central_container = lv_obj_get_child(widget->obj, 1);
        if (central_container) {
            set_battery_bar_value(central_container, status->data.battery_level, true);
        }
    } else {
        // Single device or Central without connected peripherals
        LOG_INF("⚠️  Single mode: Central only %d%% (no peripheral connected)", 
               status->data.battery_level);
        
        // Use first container for Central device
        lv_obj_t *main_container = lv_obj_get_child(widget->obj, 0);
        if (main_container) {
            set_battery_bar_value(main_container, status->data.battery_level, true);
        }
        
        // Clear the second container
        lv_obj_t *other_container = lv_obj_get_child(widget->obj, 1);
        if (other_container) {
            set_battery_bar_value(other_container, 0, false);
        }
    }
}

void zmk_widget_scanner_battery_reset(struct zmk_widget_scanner_battery *widget) {
    if (!widget || !widget->obj) {
        return;
    }
    
    LOG_INF("Battery widget reset - clearing all displays");
    
    // Clear both containers (Central and Peripheral)
    for (int i = 0; i < 2; i++) {
        lv_obj_t *container = lv_obj_get_child(widget->obj, i);
        if (container) {
            set_battery_bar_value(container, 0, false);
        }
    }
}

lv_obj_t *zmk_widget_scanner_battery_obj(struct zmk_widget_scanner_battery *widget) {
    return widget ? widget->obj : NULL;
}

#endif // CONFIG_PROSPECTOR_MODE_SCANNER && CONFIG_ZMK_DISPLAY