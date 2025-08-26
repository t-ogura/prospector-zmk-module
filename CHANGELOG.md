# Changelog

All notable changes to the prospector-zmk-module will be documented in this file.

## [v1.1.1] - 2025-08-26

### 🔒 Critical Safety Fixes
- **BREAKING**: Fixed startup failures when APDS9960 sensor hardware is not connected
- Added runtime sensor detection with graceful fallback to fixed brightness mode
- Improved Kconfig conditional compilation to prevent driver initialization errors
- Enhanced Device Tree configuration for optional hardware components

### 🧹 Repository Optimization  
- Removed 56,927 lines of unnecessary files (3D models, images, test files, CI/CD)
- Eliminated GitHub Actions workflows (ZMK modules don't need independent builds)
- Cleaned module structure to contain only essential ZMK functionality
- Removed development documentation files (CLAUDE.md, ATTRIBUTION.md, etc.)

### ⚙️ Technical Improvements
- Added compile-time hardware detection using `DT_HAS_COMPAT_STATUS_OKAY`
- Implemented safe fallback mechanisms for missing hardware
- Enhanced error handling with comprehensive logging
- Improved module structure following ZMK best practices

### 📋 Breaking Changes
- Default `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n` for safety
- Removed test configurations and build scripts
- Repository structure significantly simplified

### 🔧 Migration Guide
- Users without APDS9960 hardware: No action needed (automatic safe mode)
- Users with APDS9960 hardware: Set `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y` in config
- Update west.yml to point to v1.1.1 for the latest safety fixes

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