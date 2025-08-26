# Changelog

All notable changes to the prospector-zmk-module will be documented in this file.

## [v1.1.0] - 2025-01-30

### Added
- Scanner battery monitoring support
- APDS9960 ambient light sensor integration
- WPM (Words Per Minute) tracking with configurable windows
- RX rate display with accurate idle detection
- Configurable split keyboard central side (LEFT/RIGHT)
- Visual debug widget for development
- 15x power optimization with activity-based intervals

### Fixed
- I2C pin mapping for APDS9960 (SDA=D4, SCL=D5)
- Scanner battery cache bypass for real-time updates
- RX rate showing incorrect 5Hz during idle
- Display timeout and widget reset issues
- BLE profile detection for split keyboards
- Kconfig circular dependencies

### Changed
- Default idle interval from 2000ms to 30000ms
- Active interval from 2000ms to 100ms
- Battery widget shows 5-level color coding
- Removed debug features for production

## [v1.0.0] - 2025-01-29

### Added
- Initial stable release
- BLE advertisement protocol (26 bytes)
- YADS-style UI with pastel colors
- Multi-keyboard support (up to 3)
- Split keyboard battery display
- Real-time status monitoring
- Activity-based power management

## [v0.9.0] - 2025-01-28

### Added
- Core scanner functionality
- Basic BLE advertisement support
- Initial UI implementation
- Layer detection system

---

For detailed release notes and user documentation, please visit:
https://github.com/t-ogura/zmk-config-prospector/releases