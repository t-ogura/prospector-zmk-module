#!/bin/bash
# Build using ZMK Docker image (same as GitHub Actions)

docker run -it --rm \
  -v $(pwd):/workspace \
  -v $(pwd)/.cache:/root/.cache \
  zmkfirmware/zmk-build-arm:stable \
  sh -c "cd /workspace && west init -l config && west update && west build -d build -b seeeduino_xiao_ble -- -DSHIELD=st7789_test"