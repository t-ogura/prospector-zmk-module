# Attribution and License Information

## Original Work

This ZMK module is derived from the Prospector project by carrefinho.

**Original Repository**: https://github.com/carrefinho/prospector  
**Original Author**: carrefinho  
**Hardware License**: CERN-OHL-P-2.0 (CERN Open Hardware License - Permissive - Version 2.0)  
**Firmware License**: MIT License  

## Modifications

This repository contains modifications and extensions to the original Prospector ZMK module to support:

- BLE Advertisement-based status broadcasting
- Scanner mode for independent status display
- Multi-keyboard support
- Enhanced configuration options

## License Compliance

### MIT License (Firmware Components)

As required by the MIT License, this notice preserves the original copyright:

```
MIT License

Copyright (c) 2024 carrefinho

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### CERN-OHL-P-2.0 License (Hardware Components)

The hardware design and related documentation are licensed under CERN-OHL-P-2.0. This is a permissive license that allows:

- Commercial use
- Modification
- Distribution
- Private use

**Requirements**:
- Include copyright notice
- Include license notice
- State changes (documented in this file)

## Changes Made

The following changes were made to the original work:

1. **Added BLE Advertisement Support**:
   - `include/zmk/status_advertisement.h` - BLE advertisement protocol definition
   - `src/status_advertisement.c` - Advertisement implementation

2. **Added Scanner Mode**:
   - `src/prospector_scanner.c` - Scanner device implementation
   - `boards/shields/prospector_scanner/` - Scanner shield configuration

3. **Enhanced Configuration**:
   - Extended `Kconfig` with new configuration options
   - Added multi-keyboard support options

4. **Build System Integration**:
   - Updated for proper ZMK module integration
   - GitHub Actions support

## Acknowledgments

Special thanks to carrefinho for creating the original Prospector project and making it available under permissive licenses that enable community contributions and improvements.