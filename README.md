# Prospector ZMK Module

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ZMK Compatible](https://img.shields.io/badge/ZMK-compatible-blue)](https://zmk.dev/)

**Prospector ZMK Module** provides status display capabilities for ZMK keyboards, supporting both independent scanner mode and traditional dongle mode. This module enables real-time keyboard status broadcasting and YADS-style professional UI displays.

## 📦 What's Included

### 🖥️ **Scanner Mode Components**
- **Status Advertisement**: BLE broadcasting system for keyboard status
- **Scanner Display**: Independent status monitor with YADS-style UI
- **Multi-Keyboard Support**: Monitor multiple keyboards simultaneously
- **Professional Widgets**: Battery, layer, modifier, and connection status

### 🔌 **Dongle Mode Components** (Legacy)
- **Traditional Dongle**: Keyboard → Dongle → PC connectivity
- **Basic Status Display**: Layer roller and peripheral information
- **Legacy Support**: Compatible with original Prospector hardware

## 🏗️ Architecture

### Scanner Mode (Current - Recommended)
```
┌─────────────┐    BLE Advertisement    ┌──────────────────┐
│             │ ─────────────────────→ │   Prospector     │
│  Keyboard   │    26-byte Protocol    │   Scanner        │
│             │                        │   (USB Powered)  │
└─────────────┘                        └──────────────────┘
       │
       ├── PC (BLE/USB)
       ├── Tablet 
       ├── Phone
       └── ... (up to 5 devices)
```

### Dongle Mode (Legacy)
```
┌─────────────┐    BLE Connection    ┌──────────────────┐    USB/BLE    ┌─────┐
│  Keyboard   │ ──────────────────→ │   Prospector     │ ──────────→ │ PC  │
│ (Peripheral)│                      │   Dongle         │              └─────┘
└─────────────┘                      │  (Central)       │
                                     └──────────────────┘
```

## 🚀 Quick Start

### For Scanner Mode

#### 1. Add to Your Keyboard

Add to your keyboard's `config/west.yml`:
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
      revision: feature/yads-widget-integration
      path: modules/prospector-zmk-module
```

Add to your keyboard's `.conf` file:
```kconfig
# Enable status advertisement
CONFIG_ZMK_STATUS_ADVERTISEMENT=y
CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME="MyBoard"

# Power optimization (recommended)
CONFIG_ZMK_STATUS_ADV_ACTIVITY_BASED=y
CONFIG_ZMK_STATUS_ADV_ACTIVE_INTERVAL_MS=200    # 5Hz active
CONFIG_ZMK_STATUS_ADV_IDLE_INTERVAL_MS=1000     # 1Hz idle

# Split keyboard support (if applicable)
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
```

#### 2. Build Scanner Device

Use the companion repository: [zmk-config-prospector](https://github.com/t-ogura/zmk-config-prospector)

### For Dongle Mode (Legacy)

Add to your `build.yaml`:
```yaml
include:
  - board: seeeduino_xiao_ble
    shield: [YOUR_KEYBOARD]_dongle prospector_adapter
```

## 📡 Status Advertisement Protocol

### BLE Advertisement Format (26 bytes)

The module broadcasts keyboard status using this efficient protocol:

| Bytes | Field | Description | Example |
|-------|-------|-------------|---------|
| 0-1 | Manufacturer ID | `0xFF 0xFF` (Local use) | `FF FF` |
| 2-3 | Service UUID | `0xAB 0xCD` (Prospector) | `AB CD` |
| 4 | Protocol Version | Current: `0x01` | `01` |
| 5 | Battery Level | 0-100% (main/central) | `5A` (90%) |
| 6 | Active Layer | Current layer 0-15 | `02` (Layer 2) |
| 7 | Profile Slot | BLE profile 0-4 | `01` |
| 8 | Connection Count | Connected devices 0-5 | `03` |
| 9 | Status Flags | USB/BLE/Caps/Charging | `05` |
| 10 | Device Role | Split role indicator | `01` (Central) |
| 11 | Device Index | Split device index | `00` |
| 12-14 | Peripheral Batteries | Left/Right/Aux levels | `52 00 00` |
| 15-20 | Layer Name | Layer identifier | `4C3000...` |
| 21-24 | Keyboard ID | Hash of keyboard name | `12345678` |
| 25 | Modifier Flags | CSAG key states | `05` |

### Status Flags (Byte 9)
```
Bit 0: USB HID Ready
Bit 1: BLE Connected
Bit 2: BLE Bonded  
Bit 3: Caps Word Active
Bit 4: Charging Status
Bit 5-7: Reserved
```

### Modifier Flags (Byte 25)
```
Bit 0/4: Left/Right Ctrl
Bit 1/5: Left/Right Shift
Bit 2/6: Left/Right Alt
Bit 3/7: Left/Right GUI
```

## 🎨 Display Features

### YADS-Style UI Components

#### Battery Widget
- **Color Coding**: Green (>60%), Yellow (20-60%), Red (<20%)
- **Split Support**: Shows both central and peripheral battery levels
- **Visual Bar**: Animated progress indicator

#### Layer Widget  
- **Pastel Colors**: Each layer has unique color identity
- **Large Numbers**: Easy-to-read layer indicators (0-6)
- **Active Highlighting**: Current layer prominently displayed
- **Inactive Dimming**: Non-active layers in consistent gray

#### Modifier Widget
- **NerdFont Symbols**: Professional modifier key icons
  - Ctrl: 󰘴, Shift: 󰘶, Alt: 󰘵, GUI: 󰘳
- **Real-time Updates**: Instant modifier state changes
- **Multiple Key Support**: Shows multiple active modifiers

#### Connection Widget
- **USB Status**: Red (not ready) / White (ready)
- **BLE Status**: Green (connected) / Blue (bonded) / White (open)
- **Profile Display**: Shows active BLE profile (0-4)
- **Multi-Device**: Indicates total connected device count

## ⚙️ Configuration Options

### Advertisement Settings
```kconfig
# Basic configuration
CONFIG_ZMK_STATUS_ADVERTISEMENT=y
CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME="YourBoard"  # Keyboard identifier (any length)

# Power management
CONFIG_ZMK_STATUS_ADV_ACTIVITY_BASED=y
CONFIG_ZMK_STATUS_ADV_ACTIVE_INTERVAL_MS=200     # Active interval (ms)
CONFIG_ZMK_STATUS_ADV_IDLE_INTERVAL_MS=1000      # Idle interval (ms)
CONFIG_ZMK_STATUS_ADV_ACTIVITY_TIMEOUT_MS=2000   # Timeout to idle (ms)

# Split keyboard support
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y
```

### Scanner Device Settings
```kconfig
# Core scanner features
CONFIG_PROSPECTOR_MODE_SCANNER=y
CONFIG_PROSPECTOR_MAX_KEYBOARDS=2
CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y

# Display customization
CONFIG_PROSPECTOR_FIXED_BRIGHTNESS=80
CONFIG_PROSPECTOR_ROTATE_DISPLAY_180=n

# Font support
CONFIG_LV_FONT_MONTSERRAT_18=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LV_FONT_UNSCII_16=y
```

### Legacy Dongle Settings
```kconfig
CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=y
CONFIG_PROSPECTOR_FIXED_BRIGHTNESS=50
CONFIG_PROSPECTOR_LAYER_ROLLER_ALL_CAPS=n
```

## 🔋 Power Management

### Activity-Based Broadcasting
The module implements intelligent power management:

- **Active Mode**: 5Hz (200ms) during typing for instant responsiveness
- **Idle Mode**: 1Hz (1000ms) after 2 seconds of inactivity
- **Automatic Transitions**: Seamless switching based on keyboard activity
- **Power Impact**: ~25% increase in keyboard battery consumption

### Implementation
```c
// Automatic interval adjustment based on activity
static void update_advertisement_interval(bool active) {
    if (active) {
        // High frequency for immediate response
        schedule_next_update(CONFIG_ZMK_STATUS_ADV_ACTIVE_INTERVAL_MS);
    } else {
        // Lower frequency for battery conservation
        schedule_next_update(CONFIG_ZMK_STATUS_ADV_IDLE_INTERVAL_MS);
    }
}
```

## 🛠️ Hardware Support

### Supported Boards
- **Seeeduino XIAO BLE** (nRF52840) - Primary target
- **nice!nano v2** (nRF52840) - Compatible
- **Other nRF52840 boards** - Should work with proper pin configuration

### Display Compatibility
- **ST7789V** (240x280) - Primary support
- **ILI9341** (320x240) - Legacy support
- **Custom displays** - Possible with driver modifications

### Sensor Support  
- **APDS9960** - Ambient light sensor for auto-brightness
- **Optional sensors** - Extensible architecture for additional sensors

## 🧪 Development

### Building Locally
```bash
# Clone the module
git clone https://github.com/t-ogura/prospector-zmk-module
cd prospector-zmk-module

# Test scanner build
west init -l test/scanner_build_test
west update
west build -s zmk/app -b seeeduino_xiao_ble -- -DSHIELD=prospector_scanner

# Test dongle build
west build -s zmk/app -b seeeduino_xiao_ble -- -DSHIELD=corne_dongle_prospector_adapter
```

### Module Structure
```
prospector-zmk-module/
├── boards/shields/
│   ├── prospector_scanner/        # Scanner mode implementation
│   │   ├── src/
│   │   │   ├── scanner_display.c         # Main display logic
│   │   │   ├── scanner_battery_widget.c  # Battery visualization
│   │   │   ├── connection_status_widget.c # USB/BLE status
│   │   │   ├── layer_status_widget.c     # Layer indicators
│   │   │   ├── modifier_status_widget.c  # Modifier keys
│   │   │   └── brightness_control.c      # Auto-brightness
│   │   └── fonts/
│   │       ├── NerdFonts_Regular_20.c    # Modifier symbols
│   │       └── NerdFonts_Regular_40.c    # Large modifier symbols
│   └── prospector_adapter/        # Legacy dongle mode
├── src/
│   ├── status_advertisement.c     # BLE broadcasting
│   └── status_scanner.c          # Advertisement reception
├── include/zmk/
│   ├── status_advertisement.h
│   └── status_scanner.h
└── Kconfig                        # Configuration options
```

### API Reference

#### Advertisement API
```c
// Initialize status advertisement
int zmk_status_advertisement_init(void);

// Start broadcasting
int zmk_status_advertisement_start(void);

// Update keyboard status
int zmk_status_advertisement_update(void);
```

#### Scanner API
```c
// Initialize scanner
int zmk_status_scanner_init(void);

// Start scanning for advertisements
int zmk_status_scanner_start(void);

// Register event callback
int zmk_status_scanner_register_callback(zmk_status_scanner_callback_t callback);

// Get keyboard status
struct zmk_keyboard_status *zmk_status_scanner_get_keyboard(int index);
```

## 🐛 Debugging

### Common Issues

#### Build Errors
- **Font not found**: Enable required fonts in config
- **Shield not found**: Check module path in west.yml
- **Kconfig errors**: Verify option names and values

#### Runtime Issues
- **No advertisement**: Check `CONFIG_ZMK_STATUS_ADVERTISEMENT=y`
- **Scanner not detecting**: Verify BLE advertisement is working
- **Display blank**: Check display wiring and power

### Debug Tools

#### BLE Verification
```bash
# Use nRF Connect app to verify advertisements
# Look for manufacturer data: FF FF AB CD ...
```

#### Logging
```kconfig
# Enable detailed logging
CONFIG_LOG=y
CONFIG_ZMK_LOG_LEVEL_DBG=y
```

## 📄 License and Attribution

### Main License
This project is licensed under the **MIT License**.

### Third-Party Components

#### YADS Integration
- **License**: MIT License
- **Source**: [YADS Project](https://github.com/pashutk/yads)
- **Usage**: UI widget designs, NerdFont symbol mappings, LVGL patterns

#### Original Prospector  
- **License**: MIT License
- **Usage**: Hardware design inspiration, display driver foundation

#### ZMK Firmware
- **License**: MIT License  
- **Source**: [ZMK Project](https://github.com/zmkfirmware/zmk)
- **Usage**: Base firmware platform and APIs

#### NerdFonts
- **License**: MIT License
- **Source**: [NerdFonts](https://www.nerdfonts.com/)
- **Usage**: Modifier key symbols (󰘴󰘶󰘵󰘳)

### Font Licenses
- **Montserrat**: SIL Open Font License 1.1
- **Unscii**: Public Domain

## 🤝 Contributing

Contributions welcome! Areas for improvement:

- **New widgets**: Additional status displays
- **Hardware support**: More boards and displays  
- **Protocol extensions**: Enhanced data formats
- **Power optimizations**: Better battery management
- **UI improvements**: Themes and customization

### Development Process
1. Fork the repository
2. Create feature branch
3. Test thoroughly
4. Submit pull request
5. Follow code review process

## 📞 Support

- **Issues**: [GitHub Issues](https://github.com/t-ogura/prospector-zmk-module/issues)
- **Documentation**: [ZMK Modules Guide](https://zmk.dev/docs/features/modules)
- **Community**: [ZMK Discord](https://discord.gg/8Y8y9u5)

---

**Prospector ZMK Module** - Professional status display for ZMK keyboards.

Built with ❤️ for the ZMK community.