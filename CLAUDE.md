# Prospector ZMK Module - Development Guide

---

## 📋 重要設計ドキュメント

**再構築設計仕様書**: `/home/ogu/workspace/prospector/SCANNER_RECONSTRUCTION_DESIGN.md`

このファイルはgit管理外で、v2.0再構築の設計原則を記載しています。
困ったとき、設計判断に迷ったときは必ずこのファイルを参照してください。

### 設計の核心原則（常に遵守）

1. **ISR/CallbackからLVGL APIを呼ばない** - メッセージキューに投げるだけ
2. **共有データはMain Taskのみがアクセス** - `keyboards[]`, widget ポインタ等
3. **Work Queueを使わない** - すべての処理はMain Taskで実行
4. **mutexでLVGLスレッド安全性を確保しようとしない** - そもそもmutexが不要な設計にする

詳細は設計仕様書を参照。

---

## 🔧 Zephyr 4.x Migration Status (2025-12)

### Current Branch
- **Module**: `fix/zephyr4-boot-restoration` (prospector-zmk-module)
- **Config**: `feature/v2.0.1` (zmk-config-prospector)

### Completed Steps
| Step | Description | Status |
|------|-------------|--------|
| 1 | バックライト点灯 (SYS_INIT based) | ✅ Complete |
| 2 | BUILT_IN画面表示 | ✅ Complete |
| 3 | CUSTOM画面 (full widget test) | ✅ Complete |
| 4 | スキャナーモード (BLE advertisement) | ✅ Complete |
| 5 | I2C/タッチ/センサー | ✅ Complete |

### Build Command (Zephyr 4.x)
```bash
# IMPORTANT: Board name changed in Zephyr 4.x
# OLD: seeeduino_xiao_ble
# NEW: xiao_ble/nrf52840

cd /home/ogu/workspace/prospector/zmk-config-prospector
.venv/bin/west build -b xiao_ble/nrf52840 -s zmk/app -- \
  -DSHIELD=prospector_scanner \
  -DZMK_CONFIG="/home/ogu/workspace/prospector/zmk-config-prospector/config"
```

### Root Causes Identified
1. **Kconfig.shield excessive defaults** - ZMK_BATTERY, BT_OBSERVER等のdefault yが起動クラッシュの原因
2. **Board name change** - `seeeduino_xiao_ble` → `xiao_ble/nrf52840`
3. **I2C/Device Tree mismatch** - I2C設定とオーバーレイの不整合

### Key Files Changed
- `Kconfig.shield` - Minimized to match display_test
- `backlight_init.c` - NEW: Early GPIO initialization
- `custom_status_screen.c` - NEW: Full widget test screen
- `swipe_event.c/h` - NEW: Gesture event handling

### Reference
- **Working Reference**: `zmk-minimal-test/display_test` shield
- **Debug Notes**: `/home/ogu/workspace/prospector/zmk-config-prospector/BOOT_FIX_ATTEMPTS.md` (git管理外)

---

## 🔴 ABSOLUTE RULE: Dynamic Memory Allocation for UI Components (NEVER BREAK THIS!)

**CRITICAL PRINCIPLE**: All screen widgets MUST use dynamic memory allocation. NEVER initialize UI components at boot time.

### ✅ CORRECT Pattern: Dynamic Allocation

```c
// ✅ Settings screen - created when DOWN swipe occurs
if (!system_settings_widget) {
    system_settings_widget = zmk_widget_system_settings_create(main_screen);

    // Register LVGL input device AFTER widget is created
    touch_handler_register_lvgl_indev();
}
zmk_widget_system_settings_show(system_settings_widget);
```

### ❌ WRONG Pattern: Static/Boot-time Initialization

```c
// ❌ NEVER do this - initializing at boot time
static int scanner_display_init(void) {
    touch_handler_register_lvgl_indev();  // ← WRONG! No widgets exist yet!
    return 0;
}
SYS_INIT(scanner_display_init, APPLICATION, 60);
```

### Why This Matters

1. **Memory Efficiency**: Only allocate memory when screen is actually shown
2. **LVGL Input Device Timing**: Input devices must be registered AFTER widgets exist
3. **Clean Lifecycle**: Easy to destroy/recreate screens for memory management

### Implementation Checklist

- [ ] UI components created only when screen is shown (e.g., DOWN swipe)
- [ ] LVGL input device registered AFTER widget creation, not at boot
- [ ] Widget pointers initialized to NULL, checked before creation
- [ ] Widgets destroyed when switching screens to free memory

**If you forget this rule, buttons won't work and memory will be wasted!**

---

## ⚠️ CRITICAL: Common Build Issues (READ THIS FIRST!)

### 🔴 Font Errors - Most Common Issue

**Symptom**: Build fails with `'lv_font_montserrat_16' undeclared` or `'lv_font_unscii_16' undeclared`

**Root Cause**: LVGL fonts are NOT enabled in `prospector_scanner.conf`

**Solution**: Ensure these lines are in `config/prospector_scanner.conf`:
```conf
# Enable larger fonts for better readability
CONFIG_LV_FONT_MONTSERRAT_12=y
CONFIG_LV_FONT_MONTSERRAT_16=y
CONFIG_LV_FONT_MONTSERRAT_18=y
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_22=y
CONFIG_LV_FONT_MONTSERRAT_24=y
CONFIG_LV_FONT_MONTSERRAT_28=y

# Enable Unscii pixel fonts for retro style
CONFIG_LV_FONT_UNSCII_8=y
CONFIG_LV_FONT_UNSCII_16=y
```

**Why This Happens**:
- Widget code uses these fonts: `&lv_font_montserrat_16`, `&lv_font_unscii_16`
- If not enabled in Kconfig, they don't get compiled into LVGL
- Compiler error: undeclared identifier

**🔴 CRITICAL: Build Command Issue (2025-11-17 Root Cause Discovery)**

**Symptom**: Font errors appear even when fonts ARE configured in `prospector_scanner.conf`

**Root Cause**: Build command with `$(pwd)` shell variable NOT being expanded:
```bash
# ❌ WRONG - $(pwd) not expanded, config directory not loaded
west build ... -DZMK_CONFIG="$(pwd)/config"

# ✅ CORRECT - Use absolute path
west build ... -DZMK_CONFIG="/home/ogu/workspace/prospector/zmk-config-prospector/config"
```

**How to Verify**:
```bash
# Check build log for this line:
grep "Merged configuration" build.log

# Should see:
# Merged configuration '/home/ogu/workspace/prospector/zmk-config-prospector/config/prospector_scanner.conf'

# If you see:
# Unable to locate ZMK config at: $(pwd)/config
# ← Build command is broken!
```

**What Happens When Config Not Loaded**:
1. User config file (`prospector_scanner.conf`) with font settings is IGNORED
2. Prospector uses `ZMK_DISPLAY_STATUS_SCREEN_CUSTOM` (not built-in)
3. ZMK's default font configs (`zmk/app/src/display/Kconfig`) only apply to built-in screen
4. Result: NO fonts enabled → compilation fails

**Prevention**:
- ALWAYS use absolute paths in `-DZMK_CONFIG`
- NEVER use shell variables like `$(pwd)` in build commands
- Verify config file is loaded by checking build log
- This has been debugged multiple times - ALWAYS check the build command first!

---

### 🔴 Git Tag Creation - Use Lightweight Tags (2025-12-06)

**Symptom**: GitHub Actions fails with "undefined symbol" Kconfig errors even though the symbol exists in the module.

**Root Cause**: `west` cannot properly resolve **annotated tags**. When you create a tag with `-a` or `-m`, Git creates a tag object separate from the commit:
- Annotated tag object hash: `8004a80`
- Commit it points to: `07076ec`

West resolves the tag to the tag object hash, not the commit hash, causing module Kconfig to not be found.

**✅ CORRECT: Use Lightweight Tags**
```bash
# Lightweight tag - directly points to commit
git tag v2.0.0 07076ec
git push origin v2.0.0

# Verify it's lightweight (should show "commit")
git cat-file -t v2.0.0  # → commit
```

**❌ WRONG: Annotated Tags**
```bash
# DON'T use -a or -m flags
git tag -a v2.0.0 -m "Release message"  # ← Creates tag object, west can't resolve

# This shows "tag" instead of "commit"
git cat-file -t v2.0.0  # → tag (BAD!)
```

**How to Fix Existing Annotated Tag**:
```bash
# 1. Delete the annotated tag locally and remotely
git tag -d v2.0.0
git push origin :refs/tags/v2.0.0

# 2. Create lightweight tag at the same commit
git tag v2.0.0 <commit-hash>
git push origin v2.0.0
```

**Prevention**:
- NEVER use `git tag -a` or `git tag -m` for version tags
- ALWAYS verify with `git cat-file -t <tag>` → should show `commit`
- Both zmk-config-prospector and prospector-zmk-module tags should be lightweight

---

### 🔴 Touch Swipe Freeze - LVGL Thread Safety Issue (2025-11-18)

**Symptom**: Device freezes when performing swipe gestures on touch panel. Display stops updating, device becomes unresponsive.

**Root Cause**: LVGL is **NOT thread-safe**. Calling LVGL APIs from multiple threads causes deadlock.

**The Problem**:
```c
// ❌ WRONG - Causes deadlock!
static void touch_input_callback(struct input_event *evt) {
    // This runs in INPUT thread (from CST816S IRQ)
    if (swipe_detected) {
        k_work_submit(&bg_color_work);  // Schedule work
    }
}

static void bg_color_work_handler(struct k_work *work) {
    // This runs in WORK QUEUE thread
    lv_obj_set_style_bg_color(screen, color, 0);  // ❌ LVGL call from worker thread!
    // Meanwhile, LVGL timer also running in MAIN thread → DEADLOCK
}
```

**Why It Freezes**:
- **Main thread**: LVGL timer (`lv_task_handler`) accessing LVGL objects
- **Input thread**: CST816S IRQ processing touch events
- **Work queue thread**: Background work calling LVGL APIs
- Multiple threads access LVGL simultaneously → race condition → deadlock

**✅ SOLUTION: Use ZMK Event System**

ZMK's event system ensures thread-safe LVGL operations:

```c
// Step 1: Define custom event (src/events/swipe_gesture_event.h)
enum swipe_direction { SWIPE_DIRECTION_UP, DOWN, LEFT, RIGHT };

struct zmk_swipe_gesture_event {
    enum swipe_direction direction;
};

ZMK_EVENT_DECLARE(zmk_swipe_gesture_event);

// Step 2: Raise event from any thread (touch_handler.c)
static void touch_input_callback(struct input_event *evt) {
    // ✅ Safe - can be called from IRQ context
    if (swipe_detected) {
        raise_zmk_swipe_gesture_event(
            (struct zmk_swipe_gesture_event){.direction = SWIPE_DIRECTION_DOWN}
        );
    }
}

// Step 3: Listen in main thread (scanner_display.c)
static int swipe_gesture_listener(const zmk_event_t *eh) {
    // ✅ This runs in MAIN thread (same as LVGL timer)
    const struct zmk_swipe_gesture_event *ev = as_zmk_swipe_gesture_event(eh);

    // Safe to call LVGL APIs here!
    lv_obj_set_style_bg_color(main_screen, color, 0);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(swipe_gesture, swipe_gesture_listener);
ZMK_SUBSCRIPTION(swipe_gesture, zmk_swipe_gesture_event);
```

**Why This Works**:
1. `raise_zmk_swipe_gesture_event()` is thread-safe (uses kernel mutex)
2. Event listener **always runs in main thread** via event manager loop
3. LVGL APIs called from main thread only → no race conditions
4. Same pattern used throughout ZMK (battery events, layer events, etc.)

**Implementation Files**:
- `src/events/swipe_gesture_event.{h,c}` - Custom event definition
- `src/touch_handler.c` - Raises events from IRQ context
- `src/scanner_display.c` - Listens and calls LVGL APIs safely
- `CMakeLists.txt` - Add `swipe_gesture_event.c` to build

**Key Takeaway**:
- ⚠️ **NEVER call LVGL APIs from Work Queue, ISR, or any non-main thread without mutex**
- ✅ **ALWAYS use ZMK event system for thread-safe LVGL operations**
- 📚 Reference: ZMK's battery.c, ble.c use same pattern

---

### 🔴 LVGL Mutex Protection - Critical for Preventing Freezes (2025-11-19)

**Symptom**: Device freezes randomly during screen transitions or when multiple keyboards are being scanned.

**Root Cause**: Multiple work queues calling LVGL APIs concurrently without synchronization.

**✅ SOLUTION: Use Mutex for ALL LVGL Operations from Work Queues**

Work queues (battery updates, keyboard list updates, etc.) run in separate threads. Without mutex protection, they can interrupt each other mid-operation, causing deadlocks or corruption.

**Implementation Pattern**:

```c
// In scanner_display.c - Define mutex
static struct k_mutex lvgl_mutex;
static bool lvgl_mutex_initialized = false;

static void lvgl_mutex_init(void) {
    if (!lvgl_mutex_initialized) {
        k_mutex_init(&lvgl_mutex);
        lvgl_mutex_initialized = true;
    }
}

static int lvgl_lock(k_timeout_t timeout) {
    if (!lvgl_mutex_initialized) return -EINVAL;
    return k_mutex_lock(&lvgl_mutex, timeout);
}

static void lvgl_unlock(void) {
    if (lvgl_mutex_initialized) k_mutex_unlock(&lvgl_mutex);
}

// Global access for other files
int scanner_lvgl_lock(void) {
    return lvgl_lock(K_MSEC(100));
}

void scanner_lvgl_unlock(void) {
    lvgl_unlock();
}
```

**Work Handler Pattern**:

```c
static void my_work_handler(struct k_work *work) {
    // 1. Check swipe flag first (quick check)
    if (swipe_in_progress) {
        k_work_schedule(&my_work, K_MSEC(100));
        return;
    }

    // 2. Acquire mutex (fail = retry later)
    if (lvgl_lock(K_MSEC(50)) != 0) {
        k_work_schedule(&my_work, K_MSEC(100));
        return;
    }

    // 3. Do LVGL operations
    lv_label_set_text(label, "Hello");
    update_widget();

    // 4. Release mutex
    lvgl_unlock();

    // 5. Schedule next update
    k_work_schedule(&my_work, K_SECONDS(1));
}
```

**Swipe Handler Pattern**:

```c
static int swipe_gesture_listener(const zmk_event_t *eh) {
    swipe_in_progress = true;

    // Acquire mutex with longer timeout
    if (lvgl_lock(K_MSEC(200)) != 0) {
        swipe_in_progress = false;
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Stop all periodic work queues
    stop_all_periodic_work();

    lv_timer_enable(false);

    // Do screen transition...

    lv_timer_enable(true);

    // Resume periodic work queues if returning to main
    if (current_screen == SCREEN_MAIN) {
        resume_all_periodic_work();
    }

    lvgl_unlock();
    swipe_in_progress = false;

    return ZMK_EV_EVENT_BUBBLE;
}
```

**Critical Rules**:
1. **ALL work handlers with LVGL calls MUST acquire mutex**
2. **Swipe handlers MUST stop periodic work before transitions**
3. **Mutex timeout should be short (50ms) for work handlers, longer (200ms) for swipe**
4. **On mutex failure, reschedule work with short delay (100ms)**
5. **ALWAYS release mutex before scheduling next work**

**Files Using Mutex**:
- `scanner_display.c` - Defines mutex, swipe handler, battery work handlers
- `keyboard_list_widget.c` - Uses `scanner_lvgl_lock/unlock` for update work

**Result**: This pattern completely eliminates freeze issues even with 5+ keyboards being scanned.

---

### 🔴 Boot Crash with Sensor Disabled (CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n)

**Symptom**: Firmware boots for a moment (screen flashes) then immediately crashes when `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n`

**Root Cause**: Battery support requires SENSOR subsystem, creating a hidden dependency chain

**The "芋づる式" (Sweet Potato Vine) Configuration Chain**:
```
CONFIG_PROSPECTOR_BATTERY_SUPPORT=y (User wants battery widget)
    ↓
CONFIG_ZMK_BATTERY_REPORTING=y (Required for battery monitoring)
    ↓
select SENSOR (ZMK's Kconfig automatic selection)
    ↓
CONFIG_SENSOR=y (SENSOR subsystem enabled)
    ↓
But when CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n:
    ❌ CONFIG_I2C=n (No I2C bus)
    ❌ CONFIG_APDS9960=n (No sensor driver)
    ❌ No Device Tree sensor nodes
    ↓
SENSOR subsystem initializes but has NO hardware → CRASH
```

**Why This Happens**:
1. ZMK's `CONFIG_ZMK_BATTERY_REPORTING` **always** does `select SENSOR` (in `zmk/app/Kconfig`)
2. Battery widget needs `CONFIG_ZMK_BATTERY_REPORTING=y` to function
3. When sensor=n, I2C and sensor drivers are disabled via Kconfig
4. SENSOR subsystem tries to initialize but finds no devices → boot failure

**✅ SOLUTION IMPLEMENTED** (commit fa09ff1, 2025-11-17):

**User Goal**: Enable battery widget without light sensor hardware ("光センサーが搭載していなくてもバッテリーサポートが動く")

**Key Insight**: Decouple sensor hardware from sensor subsystem - always enable I2C/SENSOR, only disable APDS9960 when automatic brightness not needed.

**Implementation**:

1. **Kconfig** - Remove I2C/SENSOR selection from ambient light sensor:
```kconfig
config PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR
    bool "Enable ambient light sensor for automatic brightness"
    select APDS9960  # Only select the sensor driver itself
    # I2C and SENSOR now enabled separately for battery support
```

2. **Kconfig.defconfig** - Always enable I2C and SENSOR:
```kconfig
config I2C
    default y  # Always enabled for battery support

config SENSOR
    default y  # Always enabled for battery support

config APDS9960
    default n  # Only enabled when automatic brightness requested
```

3. **seeeduino_xiao_ble.overlay** - Provide I2C Device Tree:
```dts
&i2c0 {
    status = "okay";
    compatible = "nordic,nrf-twim";
    pinctrl-0 = <&i2c0_default>;  // D4=SDA, D5=SCL
    pinctrl-1 = <&i2c0_sleep>;
    clock-frequency = <I2C_BITRATE_FAST>;
};
```

4. **brightness_control.c** - Fix brightness=0 fallback:
```c
uint8_t brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;  // Use configured default
#ifdef CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB
    if (CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB > 0) {  // ← Check for 0
        brightness = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS_USB;
    }
#endif
```

**Result**:
- ✅ Battery widget works without light sensor hardware
- ✅ Automatic brightness only when `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y`
- ✅ Fixed brightness mode (65%) when sensor disabled
- ✅ Build: 897KB, boots successfully
- ✅ Configuration verified:
  - `CONFIG_I2C=y` (enabled)
  - `CONFIG_SENSOR=y` (enabled)
  - `CONFIG_APDS9960 is not set` (disabled)
  - `CONFIG_PROSPECTOR_BATTERY_SUPPORT=y` (enabled)

**User-Facing Configuration** (now works correctly):
```conf
# config/prospector_scanner.conf
CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n  # Disable auto-brightness
CONFIG_PROSPECTOR_BATTERY_SUPPORT=y           # Battery widget still works!
CONFIG_PROSPECTOR_FIXED_BRIGHTNESS=65         # Fixed brightness when sensor off
```

### 🔴 Touch Panel Configuration (v2.0.0)

**Problem**: Shield config had touch drivers always enabled, preventing non-touch builds.

**Solution** (commit 6063af5, 2025-11-24):

**Single Configuration Option**:
```conf
# config/prospector_scanner.conf
CONFIG_PROSPECTOR_TOUCH_ENABLED=y   # Touch mode (swipe navigation + settings)
# OR
CONFIG_PROSPECTOR_TOUCH_ENABLED=n   # Non-touch mode (signal widget display)
```

**How It Works**:
- Touch enabled (`=y`): Kconfig automatically selects INPUT, INPUT_CST816S, INPUT_CST816S_INTERRUPT, ZMK_POINTING
- Touch disabled (`=n`): All touch drivers disabled, signal status widget shown at bottom

**DO NOT manually set these** (auto-selected by Kconfig):
```conf
# ❌ WRONG - Don't set these manually!
# CONFIG_INPUT=y
# CONFIG_INPUT_CST816S=y
# CONFIG_INPUT_CST816S_INTERRUPT=y
# CONFIG_ZMK_POINTING=y
```

**Touch Mode Features**:
- Swipe navigation (UP/DOWN/LEFT/RIGHT)
- Display settings screen
- System settings screen
- Dynamic widget creation

**Non-Touch Mode Features**:
- Signal status widget (RSSI + reception rate)
- Display-only operation
- Smaller firmware size (~891KB vs ~1.0MB)

**Configuration Files**:
- `prospector_scanner.conf`: Default (non-touch, max_layers=7)
- `prospector_scanner_touch.conf`: Touch mode (max_layers=10)

**Build with Touch Mode**:
```bash
# Specify touch config when building
west build ... -DZMK_CONFIG_FILE=prospector_scanner_touch.conf
# OR manually edit prospector_scanner.conf to set CONFIG_PROSPECTOR_TOUCH_ENABLED=y
```

---

## Project Overview

**Prospector** is a ZMK-based status display device that shows real-time keyboard information via BLE Advertisement, without limiting keyboard connectivity.

### Key Concept
- **Non-intrusive**: Keyboards maintain full 5-device connectivity
- **BLE Advertisement**: Status broadcasting without connection
- **Multi-keyboard**: Monitor up to 3 keyboards simultaneously
- **Scanner Mode**: Independent display device (USB or battery powered)

### Hardware
- **MCU**: Seeed Studio XIAO nRF52840
- **Display**: Waveshare 1.69" Round LCD (240x280, ST7789V)
- **Sensor**: APDS9960 (optional, for auto-brightness)
- **Battery**: Optional LiPo battery support

---

## Repository Structure

### Critical: Two-Repository Architecture

```
prospector-zmk-module/          # Core module (shields, widgets, drivers)
├── boards/shields/
│   ├── prospector_scanner/     # Scanner mode shield
│   └── prospector_adapter/     # Dongle mode shield (deprecated)
├── src/                        # Core implementations
│   ├── status_advertisement.c  # BLE advertisement protocol
│   └── status_scanner.c        # BLE scanner implementation
└── Kconfig                     # Module configuration options

zmk-config-prospector/          # Build configuration
├── config/
│   ├── prospector_scanner.conf # Scanner mode config
│   ├── prospector_scanner.keymap
│   └── west.yml                # Module dependencies
└── .github/workflows/build.yml # CI/CD (optional)
```

### Repository Relationship

**prospector-zmk-module**: Shared code used by ALL Prospector devices
- Contains shield definitions, widgets, and core functionality
- Referenced by zmk-config-prospector via west.yml

**zmk-config-prospector**: Device-specific build configuration
- Contains configs, keymaps, and build settings
- Points to prospector-zmk-module branch via west.yml

---

## Development Workflow

### 🔴 CRITICAL: Always Build from Config Directory (2025-11-17)

**THE MOST IMPORTANT RULE**: ビルドは**必ずconfig側**から実行してください！

```bash
# ✅ CORRECT - Always build from zmk-config-prospector directory
cd /home/ogu/workspace/prospector/zmk-config-prospector
.venv/bin/west build -b xiao_ble/nrf52840 -s zmk/app -- \
  -DSHIELD=prospector_scanner \
  -DZMK_CONFIG="/home/ogu/workspace/prospector/zmk-config-prospector/config"

# ❌ WRONG - Never build from module directory
cd /home/ogu/workspace/prospector/prospector-zmk-module  # ← このディレクトリからビルドしない！
west build ...  # ← ビルド失敗の原因！
```

**Why This Matters** (なぜこれが重要か):
1. **Module version control**: Config側の`west.yml`でモジュールバージョンを正しく指定する必要がある
2. **Configuration loading**: Config側の`.conf`ファイルが正しく読み込まれる
3. **Build isolation**: 各プロジェクト（scanner/keyboard）が独立してビルドできる

**Workflow for Local Module Development** (ローカルモジュール開発の正しい手順):

```bash
# Step 1: モジュール側でコード変更
cd /home/ogu/workspace/prospector/prospector-zmk-module
# Edit files...
git add -A
git commit -m "Fix: something"
# ⚠️ まだpushしない - ローカルテストが先！

# Step 2: Config側のモジュールディレクトリを同じコミットに合わせる
cd /home/ogu/workspace/prospector/zmk-config-prospector/modules/prospector-zmk-module
git fetch origin  # リモートの変更を取得（ローカルコミットがあればスキップ可）
git reset --hard <commit-hash>  # 例: f180040

# Step 3: Config側からビルド実行（必ずこのディレクトリから！）
cd /home/ogu/workspace/prospector/zmk-config-prospector
rm -rf build  # クリーンビルド推奨
.venv/bin/west build -b xiao_ble/nrf52840 -s zmk/app -- \
  -DSHIELD=prospector_scanner \
  -DZMK_CONFIG="/home/ogu/workspace/prospector/zmk-config-prospector/config"

# Step 4: ビルド成功確認後にリモートへpush
cd /home/ogu/workspace/prospector/prospector-zmk-module
git push origin <branch-name>
```

**⚠️ DO NOT During Local Development**:
- ❌ `west update`を実行しない（リモートからモジュールを取得してローカル変更が消える）
- ❌ モジュール側ディレクトリからビルドしない
- ❌ テスト前にpushしない（ローカルビルドで確認してから）

### 1. Local Build Environment (Recommended)

**Setup** (one-time):
```bash
cd /home/ogu/workspace/prospector/zmk-config-prospector
python3 -m venv .venv
source .venv/bin/activate
pip install west
west init -l config
west update
```

**Build Commands** (必ずconfig側から実行):
```bash
# Scanner mode (standard) - ALWAYS use absolute path!
cd /home/ogu/workspace/prospector/zmk-config-prospector
.venv/bin/west build -b xiao_ble/nrf52840 -s zmk/app -- \
  -DSHIELD=prospector_scanner \
  -DZMK_CONFIG="/home/ogu/workspace/prospector/zmk-config-prospector/config"

# Scanner mode with USB logging
cd /home/ogu/workspace/prospector/zmk-config-prospector
.venv/bin/west build -b xiao_ble/nrf52840 -s zmk/app -- \
  -DSHIELD=prospector_scanner \
  -DZMK_CONFIG="/home/ogu/workspace/prospector/zmk-config-prospector/config" \
  -DSNIPPET="zmk-usb-logging"

# Clean build (推奨)
rm -rf build && .venv/bin/west build ...
```

**Output**: `build/zephyr/zmk.uf2` (~850-900KB)

### 2. GitHub Actions (Optional)

Builds trigger automatically on push to zmk-config-prospector, but **local builds are preferred** for faster iteration.

### 3. Git Workflow

**Module Changes** (prospector-zmk-module):
```bash
cd /home/ogu/workspace/prospector/prospector-zmk-module
# Make changes...
git add -A
git commit -m "Description"
git push
```

**Config Changes** (zmk-config-prospector):
```bash
cd /home/ogu/workspace/prospector/zmk-config-prospector
# Update west.yml if module branch changed
# OR update config files
git add -A
git commit -m "Description"
git push
```

**Important**: After module changes, rebuild zmk-config-prospector to pick up updates.

---

## Key Configuration Files

### west.yml (zmk-config-prospector/config/west.yml)

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: prospector
      url-base: https://github.com/t-ogura

  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml

    - name: prospector-zmk-module
      remote: prospector
      revision: fix/battery-hardware-detection  # Current dev branch
      path: modules/prospector-zmk-module
```

**Changing Module Branch**:
1. Update `revision:` line in west.yml
2. Run `west update` to fetch new module code
3. Rebuild firmware

### prospector_scanner.conf (Key Settings)

```conf
# Scanner Mode
CONFIG_PROSPECTOR_MODE_SCANNER=y
CONFIG_PROSPECTOR_MAX_KEYBOARDS=3

# Display
CONFIG_ZMK_DISPLAY=y
CONFIG_LVGL=y

# Brightness Control
CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n  # y if APDS9960 connected
CONFIG_PROSPECTOR_FIXED_BRIGHTNESS=85

# Battery Support (optional)
CONFIG_PROSPECTOR_BATTERY_SUPPORT=y  # Enable for battery monitoring
CONFIG_ZMK_BATTERY_REPORTING=y
CONFIG_USB_DEVICE_STACK=y

# Layer Display
CONFIG_PROSPECTOR_MAX_LAYERS=7  # Adjust to your keyboard's layer count
```

---

## BLE Advertisement Protocol

### Data Structure (26 bytes)

```c
struct zmk_status_adv_data {
    uint8_t manufacturer_id[2];      // 0xFF 0xFF
    uint8_t service_uuid[2];         // 0xAB 0xCD
    uint8_t version;                 // Protocol version
    uint8_t battery_level;           // 0-100%
    uint8_t active_layer;            // Current layer 0-15
    uint8_t profile_slot;            // BLE profile 0-4
    uint8_t connection_count;        // Connected devices
    uint8_t status_flags;            // USB/Charging/Modifier bits
    uint8_t device_role;             // CENTRAL/PERIPHERAL/STANDALONE
    uint8_t device_index;            // Split keyboard index
    uint8_t peripheral_battery[3];   // Split keyboard batteries
    uint8_t modifier_flags;          // Modifier key states
    uint8_t wpm_value;               // Words per minute
    uint8_t channel;                 // Channel number (0 = broadcast)
    uint8_t keyboard_id[4];          // Keyboard identifier
    uint8_t reserved[3];             // Future expansion
} __packed;
```

### Keyboard Integration Example

```conf
# In your keyboard's .conf file
CONFIG_ZMK_STATUS_ADVERTISEMENT=y
CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME="MyKeyboard"
CONFIG_ZMK_STATUS_ADV_INTERVAL_MS=1000

# For split keyboards with Central on LEFT side
CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE="LEFT"  # or "RIGHT" (default)
```

---

## Hardware Wiring (Scanner)

**Display (ST7789V)**:
- LCD_DIN  → Pin 10 (MOSI)
- LCD_CLK  → Pin 8  (SCK)
- LCD_CS   → Pin 9  (CS)
- LCD_DC   → Pin 7  (Data/Command)
- LCD_RST  → Pin 3  (Reset)
- LCD_BL   → Pin 6  (Backlight PWM - P0.04)

**Sensor (APDS9960, optional)**:
- VCC → 3.3V
- GND → GND
- SDA → D4 (P0.04)
- SCL → D5 (P0.05)

**Battery (XIAO BLE built-in)**:
- BAT+ / BAT- pads on XIAO
- Uses ADC channel 7 (P0.31/AIN7)
- No external wiring needed

---

## Critical Bug Fixes & Lessons Learned

### 1. Kconfig Default Value vs Config File

**Problem**: `# CONFIG_XXX=y` (commented) does NOT disable the option!

**Understanding**:
- Kconfig `default y` → Option enabled
- Config file `# CONFIG_XXX=y` → Does nothing (uses Kconfig default)
- Config file `CONFIG_XXX=n` → Explicitly disabled

**Solution**: Always use `CONFIG_XXX=n` to disable, never rely on comments.

### 2. Device Tree vs Hardware Availability

**Problem**: `DT_HAS_CHOSEN(zmk_battery)` returns true even without battery hardware.

**Root Cause**: XIAO BLE board includes battery node in device tree by default.

**Solution**: Check actual hardware with `device_is_ready()`:
```c
#if DT_HAS_CHOSEN(zmk_battery)
    const struct device *battery_dev = DEVICE_DT_GET(DT_CHOSEN(zmk_battery));
    if (device_is_ready(battery_dev)) {
        // Hardware actually available
    } else {
        // Device tree exists but hardware not ready
    }
#endif
```

### 3. Battery Widget Visibility

**Problem**: Widget never appeared even with battery connected.

**Root Causes**:
1. Widget initialization deferred to periodic handler (3s delay)
2. Initial update commented out in init function
3. Hardware check hiding widget completely

**Solution**:
- Initialize widget immediately at screen creation
- Always perform initial update in init function
- Show USB status even when battery hardware unavailable

### 4. I2C Pin Mapping (APDS9960)

**Problem**: Sensor not detected, all I2C operations failed.

**Root Cause**: SDA/SCL pins swapped in device tree overlay.

**Correct Mapping**:
```dts
&i2c0 {
    psels = <NRF_PSEL(TWIM_SDA, 0, 4)>,  // SDA = P0.04 (D4)
            <NRF_PSEL(TWIM_SCL, 0, 5)>;  // SCL = P0.05 (D5)
};
```

**Lesson**: Always verify pin mappings against official board documentation.

### 5. Split Keyboard BLE API Access

**Problem**: Build errors for LEFT keyboard - undefined BLE API references.

**Root Cause**: ZMK compiles `ble.c` only for CENTRAL role:
```cmake
if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources(app PRIVATE src/ble.c)
endif()
```

**Solution**: Conditional API calls:
```c
#if IS_ENABLED(CONFIG_ZMK_BLE) && \
    (IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT))
    uint8_t profile = zmk_ble_active_profile_index();  // Safe
#else
    uint8_t profile = 0;  // Fallback for peripheral
#endif
```

---

## Display Widgets

### Current Widgets
1. **Device Name**: Top center, large font
2. **Connection Status**: USB/BLE profile indicator
3. **Layer Status**: 0-9 layer display with pastel colors
4. **Modifier Status**: Ctrl/Alt/Shift/GUI with NerdFont icons
5. **Battery Status**: Split keyboard left/right batteries
6. **WPM**: Typing speed display
7. **Signal Status**: RSSI + reception rate
8. **Scanner Battery**: Top-right, scanner's own battery

### Widget Positioning
```
┌─────────────────────────────┐
│         Device Name      🔋 │ Scanner battery (top-right)
│ WPM                [BLE 0]  │ WPM (left), Connection (right)
│                             │
│      Layer: 0 1 2 3 4       │ Center
│     [Ctrl][Alt][Shift]      │ Modifiers
│                             │
│   ████████ 85% ████████    │ Keyboard battery
│         -45dBm 5.0Hz        │ Signal status (bottom)
└─────────────────────────────┘
```

---

## Version History

### v1.1.2 (Current Development)
**Status**: Battery widget visibility fix
**Branch**: fix/battery-hardware-detection
**Key Changes**:
- Fixed battery widget initialization timing
- Restored USB-only mode display
- Improved hardware availability checking

### v1.1.1
**Key Features**:
- APDS9960 auto-brightness with smooth fading
- Correct I2C pin mapping (SDA/SCL fix)
- Split keyboard left/right battery display

### v1.1.0
**Key Features**:
- Scanner battery widget implementation
- Multi-keyboard channel support
- Enhanced layer display (0-9 support)

### v0.9.0
**Key Features**:
- Core YADS-style UI with pastel colors
- BLE profile detection (0-4)
- Split keyboard support
- Real-time layer switching

---

## Troubleshooting

### Build Errors

**"undefined reference to zmk_ble_..."**
→ Check split role conditional compilation (see section 5 above)

**"Device Tree error: zmk_battery"**
→ Battery node exists but hardware check needed

**Font errors (lv_font_xxx)**
→ Enable font in prospector_scanner.conf: `CONFIG_LV_FONT_MONTSERRAT_XX=y`

### Runtime Issues

**Widget not visible**
→ Check widget initialization in zmk_display_status_screen()

**Sensor not detected**
→ Verify I2C pin mapping and hardware connection

**Battery shows 0%**
→ Check `device_is_ready()` and ADC configuration

### Display Issues

**Screen stays off**
→ Check backlight PWM pin (P0.04)

**Inverted brightness** (light → dark, dark → light)
→ Check sensor mapping logic in brightness_control.c

**Flickering**
→ Disable LVGL animations, check update intervals

---

## Quick Reference Commands

```bash
# Navigate to config repo
cd /home/ogu/workspace/prospector/zmk-config-prospector

# Navigate to module repo
cd /home/ogu/workspace/prospector/prospector-zmk-module

# Local build (standard)
west build -b xiao_ble/nrf52840 -s zmk/app -- \
  -DSHIELD=prospector_scanner \
  -DZMK_CONFIG="$(pwd)/config"

# Local build (with logging)
west build -b xiao_ble/nrf52840 -s zmk/app -- \
  -DSHIELD=prospector_scanner \
  -DZMK_CONFIG="$(pwd)/config" \
  -DSNIPPET="zmk-usb-logging"

# Clean build
rm -rf build

# Update module after west.yml change
west update

# View USB logs (if zmk-usb-logging enabled)
# Connect USB, firmware will output to serial console
```

---

## Development Best Practices

1. **Test locally first**: Local builds are faster than GitHub Actions
2. **Commit frequently**: Small, focused commits are easier to debug
3. **Update west.yml carefully**: Always `west update` after changing module branch
4. **Check both repos**: Module changes require config rebuild
5. **Use logging**: Enable zmk-usb-logging for debugging
6. **Verify hardware**: Always check `device_is_ready()` before hardware access
7. **Document bugs**: Record root causes and solutions for future reference

---

## 🚀 Future Feature Ideas (Left Swipe Screen)

### 時計・タイマー系
1. **BLE経由の時刻取得** - キーボードがスマホから時刻を取得し、Advertisementに含める
   - キーボード側: Current Time Service (CTS) クライアントとして時刻取得
   - Advertisement: 時刻データを status_adv_data に追加
   - Scanner側: 受信した時刻を表示
2. **セッションタイマー** - 起動からの経過時間 (`k_uptime_get()`)
3. **ストップウォッチ** - タップでスタート/ストップ/リセット
4. **ポモドーロタイマー** - 25分作業 + 5分休憩サイクル

### ビジュアル・エンターテイメント系
1. **Pong Wars** ✅ 実装中 - 2色のボールが画面を塗り合う視覚シミュレーション
2. **Matrix風の雨** - 緑の文字が流れ落ちるエフェクト
3. **Game of Life** - コンウェイのセルオートマトン
4. **バウンシングロゴ** - DVDスクリーンセーバー風
5. **パーティクル** - 花火や波紋エフェクト

### 実用系
1. **キーボード統計** - 今日の打鍵数、WPM履歴グラフ
2. **バッテリー履歴** - バッテリー残量の推移グラフ

---

**Last Updated**: 2025-01-10
**Current Branch**: fix/battery-hardware-detection
**Status**: Channel selector implementation, Pong Wars in progress
