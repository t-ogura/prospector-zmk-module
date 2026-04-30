# Prospector ZMK Module

ZMK module for Prospector status display devices.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ZMK Compatible](https://img.shields.io/badge/ZMK-compatible-blue)](https://zmk.dev/)
[![Version](https://img.shields.io/badge/version-v2.2.0-green)](https://github.com/t-ogura/zmk-config-prospector/releases)

## Overview

Prospector is a non-intrusive BLE status display for ZMK keyboards. It monitors keyboard status via BLE Advertisement without consuming a BLE connection slot — your keyboard keeps full 5-device connectivity.

- **BLE Advertisement protocol** for keyboard status broadcasting
- **Scanner mode display** with LVGL widgets (240×280 ST7789V)
- **Multi-keyboard monitoring** (up to 3 keyboards simultaneously)
- **Touch / Non-touch modes** (swipe navigation + settings, or display-only)

## Keyboard-Side Configuration

Add to your keyboard's `west.yml`:

```yaml
- name: prospector-zmk-module
  remote: prospector  # url-base: https://github.com/t-ogura
  revision: v2.2.0
  path: modules/prospector-zmk-module
```

Add to your keyboard's `.conf` file:

```conf
# Required
CONFIG_ZMK_STATUS_ADVERTISEMENT=y
CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME="MyKeyboard"

# Optional: Channel pairing (0=broadcast to all scanners)
CONFIG_PROSPECTOR_CHANNEL=0

# Optional: Activity-based intervals (enabled by default)
CONFIG_ZMK_STATUS_ADV_ACTIVITY_BASED=y
CONFIG_ZMK_STATUS_ADV_ACTIVE_INTERVAL_MS=1000
CONFIG_ZMK_STATUS_ADV_IDLE_INTERVAL_MS=30000
CONFIG_ZMK_STATUS_ADV_ACTIVITY_TIMEOUT_MS=5000

# Split keyboard: specify central side (default: RIGHT)
CONFIG_ZMK_STATUS_ADV_CENTRAL_SIDE="LEFT"

# Split keyboard: peripheral slot mapping (for 3+ device setups)
# CONFIG_ZMK_STATUS_ADV_HALF_PERIPHERAL=1
# CONFIG_ZMK_STATUS_ADV_AUX1_PERIPHERAL=0
```

### Compatibility

- **ZMK main** (Zephyr 4.x): Fully supported
- **ZMK 0.3** (Zephyr 3.5): Keyboard-side supported via compatibility macros
- Scanner requires ZMK main

## Changes in v2.2.0 (from v2.1.0)

### Bug Fixes
- **Keyboard ID collision**: Use hardware-unique ID (HWINFO) instead of name hash
- **Scanner freeze**: Restored mutex + work handler architecture with lock-free ring buffer for BT RX path
- **Kconfig FADE_STEPS duplicate**: Removed duplicate definition
- **MIPI_DBI Kconfig type**: Added missing type specification
- **I2C0 overlay conflict**: Moved to shield-level to prevent unintended activation

### New Features
- **NVS settings persistence**: Channel filter, brightness, layout settings survive reboot
- **Operator / Radii / Field layouts**: 3 new display layouts
- **CONFIG_PROSPECTOR_DEFAULT_LAYOUT**: Select startup layout via Kconfig (non-touch mode support)
- **3-battery bar display**: Operator layout auto-switches from arc to bar when 3+ peripherals
- **Layer name 4 chars**: Increased from 3 to 4 characters
- **Version protocol**: Module version embedded in BLE advertisement (displayed on scanner Quick Actions screen)
- **Boot burst**: Keyboard sends burst on startup for immediate scanner detection

### BLE Advertisement Improvements
- **Hybrid 3-mode ADV**: Piggyback on ZMK's ADV (when ZMK is advertising) / Own non-connectable ADV (when profile connected) / Connectable proxy ADV (when profile not connected, enables PC reconnection)
- **Scannable ADV**: Own ADV mode is scannable so scanner can receive keyboard name
- **KEYBOARD_NAME override**: Optional BLE device name override via config
- **Activity-based intervals**: High frequency during typing, low frequency when idle
- **Burst on events**: Layer, modifier, and profile changes trigger burst (5×15ms) for reliable delivery
- **Name-in-AD**: Periodic keyboard name broadcast in advertisement data (not just scan response)

### Scanner Improvements
- **Name display fixes**: Stuck name, flicker, and same-name collision resolved
- **High-priority change detection**: Layer/modifier/profile changes trigger immediate display update; WPM/battery updates at 1Hz
- **Channel filter**: Keyboard list filtering by channel number
- **Display refresh**: Proper redraw after screen transitions
- **Version display**: Quick Actions screen shows scanner and keyboard firmware versions

### Build Compatibility
- **Zephyr 3.5 / 4.x dual support**: SYS_INIT compatibility macros for keyboard-side code
- **LVGL 9**: Compatible (LVGL 8 API compatibility layer)
- **GitHub Actions**: Board name updated to `xiao_ble/nrf52840/zmk` for ZMK board variant

### Known Limitations
- Keyboards running v2.1.0 or earlier firmware are recognized by the v2.2.0 scanner, but profile switching may cause duplicate entries. Update keyboard firmware to v2.2.0 for full compatibility.
- Scanner firmware requires ZMK main (Zephyr 4.x). Keyboard firmware supports both ZMK 0.3 and main.

## Scanner Setup

**For complete scanner setup instructions**: [zmk-config-prospector](https://github.com/t-ogura/zmk-config-prospector)

The scanner is a standalone display device built with the `prospector_scanner` shield. It is referenced via `west.yml` in the config repository.

## Attribution

Based on the original Prospector project:
- **Hardware concept**: [prospector](https://github.com/carrefinho/prospector) by carrefinho
- **Original firmware**: [prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module)

UI design inspired by [YADS](https://github.com/janpfischer/zmk-dongle-screen).
Display layouts (Operator, Radii, Field) inspired by [carrefinho/prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module).

## License

MIT License - see [LICENSE](LICENSE) file.
