# Prospector ZMK Module

ZMK module for Prospector status display devices.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ZMK Compatible](https://img.shields.io/badge/ZMK-compatible-blue)](https://zmk.dev/)
[![Version](https://img.shields.io/badge/version-v2.2b-orange)](https://github.com/t-ogura/zmk-config-prospector/releases)

> **v2.2b** (beta): Testing release. For stable, use `v2.1.0`.

## Overview

- BLE Advertisement protocol for keyboard status broadcasting
- Scanner mode display with LVGL widgets
- Multi-keyboard monitoring support

## Changes in v2.2b (from v2.1.0)

### Bug Fixes
- **Keyboard ID collision fix**: Use hardware-unique ID (HWINFO) instead of name hash to prevent duplicate keyboard entries on BLE profile switch
- **Kconfig FADE_STEPS duplicate**: Removed duplicate definition
- **MIPI_DBI Kconfig type**: Added missing type specification
- **I2C0 overlay conflict**: Moved to shield-level to prevent unintended activation

### New Features
- **NVS settings persistence**: Channel filter, brightness, layout settings survive reboot (dirty flag + screen transition flush)
- **Operator / Radii / Field layouts**: 3 new display layouts inspired by [carrefinho/prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module)
- **CONFIG_PROSPECTOR_DEFAULT_LAYOUT**: Select startup layout via Kconfig (non-touch mode support)
- **3-battery bar display**: Operator layout dynamically switches from arc to bar when 3+ peripherals detected
- **Layer name 4 chars**: Increased from 3 to 4 characters

### BLE Advertisement Improvements
- **Hybrid 3-mode ADV**: Piggyback (disconnected) / Non-connectable (connected) / Connectable Proxy (profile switch)
- **Scannable ADV**: MODE 2 now scannable for keyboard name reception
- **KEYBOARD_NAME override**: Optional BLE device name override via config
- **Activity-based intervals**: High frequency during typing, reduced when idle

### Scanner Improvements
- **Name display fixes**: Stuck name, flicker, and same-name collision resolved
- **Channel filter**: Keyboard list filtering by channel number
- **Display refresh**: Proper redraw after screen transitions

### Build Compatibility
- **Zephyr 4.x SYS_INIT**: Updated to new signature
- **BLE name**: "Prospect" → "Prospector"

## Usage

**For complete setup instructions**: [zmk-config-prospector](https://github.com/t-ogura/zmk-config-prospector)

This module is referenced via `west.yml` - see the config repository for details.

## Attribution

Based on the original Prospector project:
- **Hardware concept**: [prospector](https://github.com/carrefinho/prospector) by carrefinho
- **Original firmware**: [prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module)

UI design inspired by [YADS](https://github.com/janpfischer/zmk-dongle-screen).
Display layouts (Operator, Radii, Field) inspired by [carrefinho/prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module).

## License

MIT License - see [LICENSE](LICENSE) file.
