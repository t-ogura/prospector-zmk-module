# Prospector ZMK Module

All the necessary stuff for [Prospector](https://github.com/carrefinho/prospector) to display things with ZMK. 

## Features

### Dongle Mode (Original)
- Highest active layer roller
- Peripheral battery bar
- Peripheral connection status
- Caps word indicator

### Scanner Mode (New)
- **Independent status display device**
- BLE Advertisement scanning for keyboard status
- Multiple keyboard support (up to 3 simultaneously)
- Works with any ZMK keyboard without limiting connections
- Automatic brightness control with ambient light sensor
- USB-powered or battery operation

### Status Advertisement (New)
- **Keyboard status broadcasting via BLE Advertisement**
- Non-intrusive - doesn't affect keyboard's normal operation
- Broadcasts battery, layer, connection status
- Configurable update interval and keyboard identification

## Installation

### Option 1: Scanner Mode (Recommended)
Use Prospector as an independent status display device.

#### Keyboard Side
Add this module to your keyboard's `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: t-ogura
      url-base: https://github.com/t-ogura
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: prospector-zmk-module
      remote: t-ogura
      revision: feature/scanner-mode
  self:
    path: config
```

Add to your keyboard's `.conf` file:
```conf
CONFIG_ZMK_STATUS_ADVERTISEMENT=y
CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME="MyKeyboard"
```

#### Prospector Side
Use the `prospector_scanner` shield:

```yaml
include:
  - board: seeeduino_xiao_ble
    shield: prospector_scanner
```

### Option 2: Dongle Mode (Original)
Your ZMK keyboard should be set up with a dongle as central.

Add this module to your `config/west.yml` with these new entries under `remotes` and `projects`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: carrefinho                            # <--- add this
      url-base: https://github.com/carrefinho     # <--- and this
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: prospector-zmk-module                 # <--- and these
      remote: carrefinho                          # <---
      revision: main                              # <---
  self:
    path: config
```

Then add the `prospector_adapter` shield to the dongle in your `build.yaml`:

```yaml
---
include:
  - board: seeeduino_xiao_ble
    shield: [YOUR KEYBOARD SHIELD]_dongle prospector_adapter
```

For more information on ZMK Modules and building locally, see [the ZMK docs page on modules.](https://zmk.dev/docs/features/modules)

## Usage

For split keyboards, since the peripheral battery widget uses the order in which peripherals were paired to arrange the sub-widgets, after flashing the dongle, pair the left side first and then the right side. For more than two peripherals, pair them in a left to right order.

The layer roller shows layers' `display-name` property whenever available, and will fall back to the layer index otherwise. To add a `display-name` property to a keymap layer:

```dts
keymap {
  compatible = "zmk,keymap";
  base {
    display-name = "Base";           # <--- add this
    bindings = <
      ...
    >;
  }
}
```

## Configuration

To customize, add config options to your `config/[YOUR KEYBOARD SHIELD].conf` like so:
```ini
CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR=n
CONFIG_PROSPECTOR_FIXED_BRIGHTNESS=80
```

### Available config options:
| Name                                              | Description                                                               | Default      |
| ------------------------------------------------- | --------------------------------------------------------------------------| ------------ |
| `CONFIG_PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR`      | Use ambient light sensor for auto brightness, set to `n` if building without one                              | y            |
| `CONFIG_PROSPECTOR_FIXED_BRIGHTESS`               | Set fixed display brightess when not using ambient light sensor           | 50 (1-100)   |
| `CONFIG_PROSPECTOR_PROSPECTOR_ROTATE_DISPLAY_180` | Rotate the display 180 degrees                                            | n            |
| `CONFIG_PROSPECTOR_LAYER_ROLLER_ALL_CAPS`         | Convert layer names to all caps                                           | n            |# Trigger build for latest prospector module updates
