# LVGL 9 Container + Label Freeze Bug Report

## Summary

Setting text on a label that is a child of a container object (`lv_obj_create()`) causes the system to freeze on Zephyr 4.1 with LVGL 9.x. Labels created directly on the screen (`lv_obj_create(NULL)`) work correctly.

**Severity**: Critical (system freeze, no recovery without power cycle)

**Date Discovered**: 2025-12-17

---

## Environment

| Component | Version |
|-----------|---------|
| Zephyr RTOS | 4.1 (from ZMK main branch) |
| LVGL | 9.x (bundled with Zephyr) |
| ZMK Firmware | main branch (post-Zephyr 4.1 migration) |
| Hardware | Seeed XIAO nRF52840 (BLE) |
| Display | Waveshare 1.69" LCD (ST7789V, 240x280, SPI) |
| Build Date | 2025-12-17 |

### Relevant Kconfig Settings

```conf
CONFIG_ZMK_DISPLAY=y
CONFIG_DISPLAY=y
CONFIG_LVGL=y
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y
CONFIG_LV_Z_MEM_POOL_SIZE=8192
CONFIG_LV_FONT_MONTSERRAT_16=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_USE_LABEL=y
CONFIG_LV_USE_FLEX=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_COLOR_16_SWAP=y
```

---

## Bug Description

### What Works (No Freeze)

```c
// Pattern 1: Label directly on screen - WORKS
lv_obj_t *screen = lv_obj_create(NULL);
lv_obj_t *label = lv_label_create(screen);
lv_label_set_text(label, "Hello");  // OK

// Pattern 2: Multiple labels on screen - WORKS
lv_obj_t *label1 = lv_label_create(screen);
lv_obj_t *label2 = lv_label_create(screen);
lv_label_set_text(label1, "First");   // OK
lv_label_set_text(label2, "Second");  // OK

// Pattern 3: Empty container - WORKS
lv_obj_t *container = lv_obj_create(screen);
lv_obj_set_size(container, 100, 50);
// No children - OK

// Pattern 4: Label in container without text - WORKS
lv_obj_t *container = lv_obj_create(screen);
lv_obj_t *label = lv_label_create(container);
// No lv_label_set_text() call - OK
```

### What Freezes

```c
// Pattern that causes freeze:
lv_obj_t *screen = lv_obj_create(NULL);
lv_obj_t *container = lv_obj_create(screen);  // Container as child of screen
lv_obj_t *label = lv_label_create(container); // Label as child of container
lv_label_set_text(label, "Hello");            // <-- FREEZE HERE
```

### Variations Tested (All Still Freeze)

```c
// Attempt 1: Disable scrolling on container
lv_obj_t *container = lv_obj_create(screen);
lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
lv_obj_t *label = lv_label_create(container);
lv_label_set_text(label, "Hello");  // FREEZE

// Attempt 2: Set fixed size on label before text
lv_obj_t *label = lv_label_create(container);
lv_obj_set_size(label, 80, 20);
lv_label_set_text(label, "Hello");  // FREEZE

// Attempt 3: Set long mode to clip (disable auto-resize)
lv_obj_t *label = lv_label_create(container);
lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
lv_label_set_text(label, "Hello");  // FREEZE

// Attempt 4: Set font before text
lv_obj_t *label = lv_label_create(container);
lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
lv_label_set_text(label, "Hello");  // FREEZE

// Attempt 5: Enable CONFIG_LV_USE_FLEX=y
// Still freezes

// Attempt 6: Use static text
lv_label_set_text_static(label, "Hello");  // Likely FREEZE (not tested separately)
```

---

## Minimal Reproduction Code

```c
#include <lvgl.h>
#include <zmk/display/status_screen.h>

LV_FONT_DECLARE(lv_font_montserrat_16);

lv_obj_t *zmk_display_status_screen(void) {
    // Create screen
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Create container
    lv_obj_t *container = lv_obj_create(screen);
    lv_obj_set_size(container, 100, 50);
    lv_obj_center(container);

    // Create label in container - THIS CAUSES FREEZE
    lv_obj_t *label = lv_label_create(container);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_label_set_text(label, "HELLO");  // <-- System freezes here

    return screen;
}
```

---

## Investigation Findings

### 1. LVGL Label Default Behavior

From `lv_label.c`:
```c
const lv_obj_class_t lv_label_class = {
    .width_def = LV_SIZE_CONTENT,   // Labels auto-size to content
    .height_def = LV_SIZE_CONTENT,
    // ...
};
```

Labels default to `LV_SIZE_CONTENT`, meaning they resize based on text content.

### 2. Text Setting Triggers Layout Refresh

From `lv_label.c`:
```c
void lv_label_set_text(lv_obj_t * obj, const char * text) {
    // ... text processing ...
    lv_label_refr_text(obj);  // Triggers layout recalculation
}

static void lv_label_refr_text(lv_obj_t * obj) {
    // ...
    lv_text_get_size(&size, label->text, font, ...);
    lv_obj_refresh_self_size(obj);  // <-- Likely trigger point
    // ...
}
```

### 3. Default Object Flags

From `lv_obj.c`:
```c
// Default flags for lv_obj_create():
obj->flags = LV_OBJ_FLAG_CLICKABLE;
obj->flags |= LV_OBJ_FLAG_SCROLLABLE;      // Scrollable by default
obj->flags |= LV_OBJ_FLAG_SCROLL_ELASTIC;
obj->flags |= LV_OBJ_FLAG_SCROLL_MOMENTUM;
// ...
```

Containers are scrollable by default, which may interact badly with child size changes.

### 4. LVGL 9 Breaking Changes (Potentially Related)

From LVGL 9.0 changelog:
- `lv_coord_t` removed, replaced by `int32_t`
- `lv_color_t` always stored as RGB888 internally
- Layout system changes
- `fix(obj): fix crash with LV_SIZE_CONTENT parent and % positioned child`

The last item suggests there were known issues with `LV_SIZE_CONTENT` and parent-child relationships.

---

## Working Workaround

Create all labels directly on the screen object instead of using containers:

```c
lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // All labels as direct children of screen - NO CONTAINERS
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Hello");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "World");
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 0);

    return screen;
}
```

### ZMK Examples Using This Pattern

1. **ZMK Built-in Status Screen** (`zmk/app/src/display/status_screen.c`):
   - Creates all widgets directly on screen
   - No intermediate containers

2. **nice_view Shield** (`zmk/app/boards/shields/nice_view/`):
   - Uses `lv_canvas` with `lv_draw_label()` for text
   - Completely avoids label widget parent-child relationships

---

## Impact on Prospector Scanner

The Prospector Scanner module uses this pattern extensively:

```c
// Current pattern (FREEZES in LVGL 9):
widget->obj = lv_obj_create(parent);           // Container
widget->title = lv_label_create(widget->obj);  // Label in container
lv_label_set_text(widget->title, "Layer");     // FREEZE
```

**Affected files** (12 widget files):
- `layer_status_widget.c`
- `connection_status_widget.c`
- `modifier_status_widget.c`
- `wpm_status_widget.c`
- `scanner_battery_widget.c`
- `scanner_battery_status_widget.c`
- `signal_status_widget.c`
- `keyboard_list_widget.c`
- `display_settings_widget.c`
- `system_settings_widget.c`
- `profile_status_widget.c`
- `debug_status_widget.c`

**Migration Required**: All widgets need to be refactored to create labels directly on the parent screen instead of using container objects.

---

## Possible Root Causes (Speculation)

1. **Layout recursion**: Setting label text triggers size recalculation, which triggers parent (container) layout update, which may trigger infinite recursion or deadlock.

2. **Memory allocation during layout**: `lv_label_set_text()` allocates memory for text. If this happens during a layout pass that's already allocating, it could cause heap corruption or deadlock.

3. **Scroll calculation deadlock**: Container's default scrollable flag may trigger scroll area recalculation when child size changes, causing deadlock with LVGL's internal state.

4. **Zephyr LVGL integration issue**: The Zephyr-specific LVGL port may have threading or memory management issues not present in other LVGL integrations.

---

## Recommended Actions

### Short-term (Workaround)
- Refactor all widgets to use "flat" structure (labels directly on screen)
- Use `lv_obj_align()` for positioning instead of container-relative layout

### Long-term (Bug Report)
- Report to LVGL GitHub with minimal reproduction case
- Report to Zephyr project if Zephyr-specific
- Include all environment details and test findings

---

## Test Environment Setup

Repository: `zmk-minimal-test`

```bash
# Build command
cd /home/ogu/workspace/prospector/zmk-minimal-test
.venv/bin/west build -b xiao_ble -s zmk/app -- \
  -DSHIELD=display_test \
  -DZMK_CONFIG="/home/ogu/workspace/prospector/zmk-minimal-test/config"

# Output
build/zephyr/zmk.uf2
```

Test files:
- `config/boards/shields/display_test/custom_status_screen.c` - Main test file
- `config/boards/shields/display_test/display_test.conf` - Kconfig
- `config/boards/shields/display_test/display_test.overlay` - Device tree

---

## References

- LVGL 9.0 Migration Guide: https://docs.lvgl.io/master/CHANGELOG.html
- ZMK Display Documentation: https://zmk.dev/docs/features/displays
- Zephyr LVGL Module: `zephyr/modules/lvgl/`

---

## Document History

| Date | Author | Description |
|------|--------|-------------|
| 2025-12-17 | Claude + User | Initial investigation and documentation |
