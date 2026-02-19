/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Display Settings - NVS Persistence
 *
 * Uses Zephyr Settings Subsystem to persist display settings to flash.
 * Follows the same pattern as ZMK core (rgb_underglow.c, endpoints.c).
 *
 * NVS key structure:
 *   "prosp/brightness"  - auto_brightness (bool) + manual_brightness (uint8_t)
 *   "prosp/layers"      - max_layers (uint8_t) + slide_mode (bool)
 *   "prosp/channel"     - scanner channel (uint8_t)
 *   "prosp/layout"      - layout style (uint8_t)
 */

#include "display_settings.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display_settings, CONFIG_ZMK_LOG_LEVEL);

/* ========== Persisted State Structures ========== */

struct brightness_settings {
    bool auto_enabled;
    uint8_t manual_level;
} __packed;

struct layer_settings {
    uint8_t max_layers;
    bool slide_mode;
} __packed;

/* ========== Default Values from Kconfig ========== */

static struct brightness_settings brightness = {
    .auto_enabled = false,
#ifdef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS
    .manual_level = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS,
#else
    .manual_level = 65,
#endif
};

static struct layer_settings layers = {
#ifdef CONFIG_PROSPECTOR_MAX_LAYERS
    .max_layers = CONFIG_PROSPECTOR_MAX_LAYERS,
#else
    .max_layers = 7,
#endif
    .slide_mode = IS_ENABLED(CONFIG_PROSPECTOR_LAYER_SLIDE_DEFAULT),
};

static uint8_t scanner_channel =
#ifdef CONFIG_PROSPECTOR_SCANNER_CHANNEL
    CONFIG_PROSPECTOR_SCANNER_CHANNEL;
#else
    0;
#endif

static uint8_t layout_style = 2; /* PROSPECTOR_LAYOUT_OPERATOR = 2 */

static bool settings_loaded = false;

/* ========== Settings Load Handler ========== */

#if IS_ENABLED(CONFIG_SETTINGS)

static int display_settings_handle_set(const char *name, size_t len,
                                       settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "brightness", &next) && !next) {
        if (len != sizeof(brightness)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &brightness, sizeof(brightness));
        if (rc >= 0) {
            LOG_INF("Loaded brightness: auto=%d, manual=%d",
                    brightness.auto_enabled, brightness.manual_level);
            return 0;
        }
        return rc;
    }

    if (settings_name_steq(name, "layers", &next) && !next) {
        if (len != sizeof(layers)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &layers, sizeof(layers));
        if (rc >= 0) {
            LOG_INF("Loaded layers: max=%d, slide=%d",
                    layers.max_layers, layers.slide_mode);
            return 0;
        }
        return rc;
    }

    if (settings_name_steq(name, "channel", &next) && !next) {
        if (len != sizeof(scanner_channel)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &scanner_channel, sizeof(scanner_channel));
        if (rc >= 0) {
            LOG_INF("Loaded channel: %d", scanner_channel);
            return 0;
        }
        return rc;
    }

    if (settings_name_steq(name, "layout", &next) && !next) {
        if (len != sizeof(layout_style)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &layout_style, sizeof(layout_style));
        if (rc >= 0) {
            LOG_INF("Loaded layout: %d", layout_style);
            return 0;
        }
        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(prosp_display, "prosp", NULL,
                               display_settings_handle_set, NULL, NULL);

/* ========== Debounced Save ========== */

static void display_settings_save_work_handler(struct k_work *work) {
    settings_save_one("prosp/brightness", &brightness, sizeof(brightness));
    settings_save_one("prosp/layers", &layers, sizeof(layers));
    settings_save_one("prosp/channel", &scanner_channel, sizeof(scanner_channel));
    settings_save_one("prosp/layout", &layout_style, sizeof(layout_style));
    LOG_INF("Display settings saved to NVS");
}

static struct k_work_delayable save_work;

/* Display settings use a shorter debounce than ZMK default (60s).
 * User expects settings to persist immediately after changing them.
 * 2 seconds is enough to batch rapid slider adjustments. */
#define DISPLAY_SETTINGS_SAVE_DEBOUNCE_MS 2000

static void schedule_save(void) {
    if (!settings_loaded) {
        return; /* Don't save before initial load completes */
    }
    k_work_reschedule(&save_work, K_MSEC(DISPLAY_SETTINGS_SAVE_DEBOUNCE_MS));
}

#else /* !CONFIG_SETTINGS */

static void schedule_save(void) {}

#endif /* CONFIG_SETTINGS */

/* ========== Initialization ========== */

void display_settings_init(void) {
    if (settings_loaded) {
        return;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&save_work, display_settings_save_work_handler);

    /* Load saved settings from NVS.
     * settings_load() is called by ZMK main.c, which loads ALL registered
     * handlers including ours. But we call load_subtree for our prefix
     * explicitly to be safe in case init order varies. */
    settings_load_subtree("prosp");
#endif

    settings_loaded = true;
    LOG_INF("Display settings initialized: bright=%d/%d%%, layers=%d/%s, ch=%d, layout=%d",
            brightness.auto_enabled, brightness.manual_level,
            layers.max_layers, layers.slide_mode ? "slide" : "list",
            scanner_channel, layout_style);
}

/* ========== Brightness Getters/Setters ========== */

bool display_settings_get_auto_brightness(void) {
    return brightness.auto_enabled;
}

void display_settings_set_auto_brightness(bool enabled) {
    if (brightness.auto_enabled == enabled) {
        return;
    }
    brightness.auto_enabled = enabled;
    schedule_save();
}

uint8_t display_settings_get_manual_brightness(void) {
    return brightness.manual_level;
}

void display_settings_set_manual_brightness(uint8_t level) {
    if (brightness.manual_level == level) {
        return;
    }
    brightness.manual_level = level;
    schedule_save();
}

/* ========== Layer Getters/Setters ========== */

uint8_t display_settings_get_max_layers(void) {
    return layers.max_layers;
}

void display_settings_set_max_layers(uint8_t max) {
    if (layers.max_layers == max) {
        return;
    }
    layers.max_layers = max;
    schedule_save();
}

bool display_settings_get_layer_slide_mode(void) {
    return layers.slide_mode;
}

void display_settings_set_layer_slide_mode(bool enabled) {
    if (layers.slide_mode == enabled) {
        return;
    }
    layers.slide_mode = enabled;
    schedule_save();
}

/* ========== Channel Getters/Setters ========== */

uint8_t display_settings_get_channel(void) {
    return scanner_channel;
}

void display_settings_set_channel(uint8_t channel) {
    if (scanner_channel == channel) {
        return;
    }
    scanner_channel = channel;
    schedule_save();
}

/* ========== Layout Getters/Setters ========== */

uint8_t display_settings_get_layout(void) {
    return layout_style;
}

void display_settings_set_layout(uint8_t layout) {
    if (layout_style == layout) {
        return;
    }
    layout_style = layout;
    schedule_save();
}
