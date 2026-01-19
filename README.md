# Prospector ZMK Module

ZMK module for Prospector status display devices.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ZMK Compatible](https://img.shields.io/badge/ZMK-compatible-blue)](https://zmk.dev/)
[![Version](https://img.shields.io/badge/version-v2.1.0-brightgreen)](https://github.com/t-ogura/zmk-config-prospector/releases)

> **v2.1.0**: Zephyr 4.x / ZMK main branch compatible. For ZMK ≤0.3 (Zephyr 3.x), use `v2.0.0`.

## Overview

- BLE Advertisement protocol for keyboard status broadcasting
- Scanner mode display with LVGL widgets
- Multi-keyboard monitoring support

## Usage

**For complete setup instructions**: [zmk-config-prospector](https://github.com/t-ogura/zmk-config-prospector)

This module is referenced via `west.yml` - see the config repository for details.

## Attribution

Based on the original Prospector project:
- **Hardware concept**: [prospector](https://github.com/carrefinho/prospector) by carrefinho
- **Original firmware**: [prospector-zmk-module](https://github.com/carrefinho/prospector-zmk-module)

UI design inspired by [YADS](https://github.com/janpfischer/zmk-dongle-screen).

## License

MIT License - see [LICENSE](LICENSE) file.
