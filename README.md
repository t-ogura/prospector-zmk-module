# Prospector

Prospector is a desktop ZMK dongle with full color LCD screen.

![Prospector between split keyboards on white background](/docs/images/prospector_hero2.jpg)

> not doing a great job showcasing those 16-bit colors am i

Shown here alongside [Forager](https://github.com/carrefinho/forager).

## Design

- 1.69-inch IPS LCD screen with curved cover glass
- Auto brightness with ambient light sensor
- 3D-printed case with externally accessible reset button

## Build Guide

[Find the Assembly Manual here.](/docs/prospector_assembly_manual.jpg)

## Firmware & Usage

[Find the ZMK module and instructions here.](https://github.com/carrefinho/prospector-zmk-module)

## Bill of Materials

| Part Name | Part Number | Count | Notes |
| --------- | ----------- | ----- | ----- |
| 3D-printed case | - | 1 | [Find STLs here](./case/) |
| Seeed Studio XIAO nRF52840 | - | 1 | https://www.seeedstudio.com/Seeed-XIAO-BLE-nRF52840-p-5201.html |
| Waveshare 1.69inch Round LCD Display Module **with Touch** | 27057 | 1 | https://www.waveshare.com/1.69inch-touch-lcd-module.htm<br>Non-touch version has different mounting pattern and will **NOT** fit|
| Adafruit APDS9960 Proximity, Light, RGB, and Gesture Sensor | 3595 | 1 | https://www.adafruit.com/product/3595<br>Optional, `no_als` case file available. |
| M2x6 pan/wafer head screws | - | 4 |  |
| M2.5x4 pan/wafer head screws | - | 4 |  |

## Credits

- [englmaxi\/zmk-dongle-display](https://github.com/englmaxi/zmk-dongle-display): inspiration, display code, module docs
- [badjeff's many modules](https://github.com/badjeff): CMake magic
- [JetKVM](https://jetkvm.com/): form factor
- [caksoylar\/zmk-rgbled-widget](https://github.com/caksoylar/zmk-rgbled-widget): module docs
- IKEA(???): assembly manual design