# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Prospector is an **open-source hardware project** for a desktop ZMK dongle with a full-color LCD display. It provides a visual interface for ZMK-based mechanical keyboards.

## Current Project Goal - Independent Status Display Device

### 背景と課題
**Original Design**: Prospectorは元々ZMKキーボード用のドングルとして設計され、USBドングル機能とステータス表示機能を提供していました。

**問題点**: ドングルモードでは、キーボードがProspectorにしか接続できず、本来の複数デバイス接続機能（最大5台）が失われてしまいます。

### 新しい仕様：独立型ステータス表示デバイス

**コンセプト**: キーボードの機能を一切制限せず、オプショナルなアクセサリーとして動作する

**システム構成**:
1. **キーボード側**: 
   - 通常通り最大5台のデバイスに接続可能
   - BLE Advertisementでステータス情報をブロードキャスト
   - Prospectorの有無に関わらず完全に通常動作

2. **Prospector側**:
   - 独立したBLEスキャナーとして動作
   - USB給電（将来的にバッテリー駆動も可）
   - 複数のキーボードから同時受信可能
   - PCとの接続不要

**技術仕様**:
- 通信方式: BLE Advertisement（非接続型）
- データ更新間隔: 1秒
- 最大データサイズ: 31バイト
- 電源: USB Type-C（5V給電）

**Key Components:**
- Seeed Studio XIAO nRF52840 microcontroller
- Waveshare 1.69" Round LCD Display Module (with touch)
- Optional Adafruit APDS9960 sensor for auto-brightness
- 3D-printed case

## Repository Structure

- `/case/` - 3D printable STL files and CAD source files (Fusion 360)
  - `prospector.f3z` - Original Fusion 360 design file
  - `prospector.step` - STEP file for CAD interoperability
  - `main_body.stl` - Primary case with sensor cutout
  - `main_body_no_sensor.stl` - Alternative case without sensor
  - `disp_mount.stl` - Display mounting bracket
  - `rear_cap.stl` - Back cover with reset button access
  - `sensor_light_guide.stl` - Light pipe for ambient sensor (optional)
- `/docs/` - Documentation and assembly manual
  - `prospector_assembly_manual.jpg` - Visual build guide
  - `/images/` - Product photos
- **Firmware is in a separate repository**: https://github.com/carrefinho/prospector-zmk-module

## Development Commands

This is a hardware design repository with no build commands. Development workflow:

1. **Hardware Design Modifications**:
   - Edit in Autodesk Fusion 360 using `case/prospector.f3z`
   - Export updated STL files for 3D printing
   - Export STEP file for CAD interoperability

2. **Documentation Updates**:
   - Assembly manual is at `docs/prospector_assembly_manual.jpg`
   - Product photos in `docs/images/`

## Architecture and Design Considerations

**Hardware Architecture:**
- Desktop form factor (not embedded in keyboard)
- Uses standard maker-friendly components (Adafruit, Seeed Studio, Waveshare)
- Modular design with optional ambient light sensor
- Two case variants: with and without sensor cutout

**Firmware Integration:**
- This repository contains only the hardware design
- Firmware development happens in the separate ZMK module repository
- The hardware exposes standard SPI interface for the display
- I2C interface for the optional APDS9960 sensor

**License:**
- CERN Open Hardware License v2 - Permissive (CERN-OHL-P)
- Allows commercial use and modifications

## Important Notes

- When modifying STL files, ensure they remain 3D-printable without supports
- The display module MUST be the touch version (Waveshare 27057) due to mounting hole pattern
- Case design assumes M2x6 and M2.5x4 screws for assembly
- The reset button must remain externally accessible for firmware updates

## Common Maintenance Tasks

- **Updating Case Design**: Modify `prospector.f3z` in Fusion 360, then export new STL files
- **Fixing Wiring Issues**: Recent commits show wiring diagram updates - ensure documentation stays in sync
- **Adding Case Variants**: Follow the pattern of `main_body.stl` vs `main_body_no_sensor.stl` for modularity

## Implementation Details - BLE Advertisement方式

### データ構造定義（31バイト）
```c
// Advertisement Packet Structure
struct zmk_status_adv_data {
    uint8_t manufacturer_id[2];  // 0xFF, 0xFF (2バイト)
    uint8_t service_uuid[2];     // 0xABCD (仮) - Prospector識別用 (2バイト)
    uint8_t version;             // プロトコルバージョン (1バイト)
    uint8_t battery_level;       // 0-100% (1バイト)
    uint8_t active_layer;        // 現在のレイヤー番号 0-15 (1バイト)
    uint8_t profile_slot;        // アクティブプロファイル 0-4 (1バイト)
    uint8_t connection_count;    // 接続中のデバイス数 0-5 (1バイト)
    uint8_t status_flags;        // ビットフラグ (1バイト)
        // bit 0: caps_word
        // bit 1: charging
        // bit 2: usb_connected
        // bit 3-7: 予約
    char layer_name[8];          // レイヤー名（NULL終端） (8バイト)
    uint8_t keyboard_id[4];      // キーボード識別子 (4バイト)
    uint8_t reserved[8];         // 将来の拡張用 (8バイト)
}; // 合計: 30バイト
```

### 実装ファイル構成

#### キーボード側（prospector-zmk-module）
```
prospector-zmk-module/
├── src/
│   ├── behaviors/
│   │   └── behavior_status_adv.c    # Advertisement送信behavior
│   ├── status/
│   │   └── zmk_status_broadcaster.c  # ステータス収集・送信
│   └── Kconfig                       # 設定項目追加
├── dts/
│   └── bindings/
│       └── zmk,status-advertisement.yaml
└── CMakeLists.txt
```

#### Prospector側（新規追加）
```
prospector-zmk-module/
├── boards/
│   └── shields/
│       └── prospector_scanner/
│           ├── prospector_scanner.conf
│           ├── prospector_scanner.overlay
│           └── Kconfig.shield
└── src/
    └── scanner/
        ├── ble_scanner.c         # BLEスキャン実装
        └── status_receiver.c     # データ受信・UI更新

```

### 設定ファイル例

#### キーボード側設定（zmk-config/config/corne.conf）
```conf
# Enable Prospector Status Advertisement
CONFIG_ZMK_STATUS_ADVERTISEMENT=y
CONFIG_ZMK_STATUS_ADV_INTERVAL_MS=1000  # 1秒ごと
CONFIG_ZMK_STATUS_ADV_KEYBOARD_NAME="Corne"
```

#### Prospector側設定（prospector_scanner.conf）
```conf
# Scanner Mode Configuration
CONFIG_PROSPECTOR_MODE_SCANNER=y
CONFIG_BT_OBSERVER=y
CONFIG_BT_SCAN_FILTER_ENABLE=y
CONFIG_BT_SCAN_WINDOW=100      # 100ms
CONFIG_BT_SCAN_INTERVAL=200    # 200ms

# Display Configuration
CONFIG_ZMK_DISPLAY=y
CONFIG_PROSPECTOR_ALS=y        # 自動輝度調整
CONFIG_PROSPECTOR_WIDGET_BATTERY_BAR=y
CONFIG_PROSPECTOR_WIDGET_LAYER_ROLLER=y
CONFIG_PROSPECTOR_WIDGET_CAPS_WORD=y

# Scanner Features
CONFIG_PROSPECTOR_MULTI_KEYBOARD=y     # 複数キーボード対応
CONFIG_PROSPECTOR_MAX_KEYBOARDS=3      # 最大3台まで表示
```

## User Setup Guide - ユーザー向けセットアップ

### Phase 1: キーボード側の設定
1. 既存のzmk-configリポジトリを開く
2. `config/west.yml`に以下を追加：
   ```yaml
   - name: prospector-zmk-module
     remote: carrefinho
     revision: scanner-support
   ```
3. キーボードの`.conf`ファイルに追加：
   ```conf
   CONFIG_ZMK_STATUS_ADVERTISEMENT=y
   ```
4. 通常通りビルド・書き込み

### Phase 2: Prospector側の設定
1. `zmk-config-prospector`テンプレートをフォーク
2. 必要に応じて`prospector_scanner.conf`を編集
3. GitHub Actionsでビルド（自動）
4. Artifactsから`prospector_scanner.uf2`をダウンロード
5. ProspectorをBootloaderモードにして書き込み

### Phase 3: 使用方法
1. ProspectorをUSB電源に接続（PCでもUSB充電器でもOK）
2. 自動的にキーボードを検出・表示開始
3. 複数のキーボードがある場合は自動で切り替え表示

## Development Roadmap - 開発ロードマップ

### Step 1: Core Implementation（現在の目標）
- [ ] Advertisement送信機能の実装
- [ ] Scanner受信機能の実装
- [ ] 既存UIの流用・調整
- [ ] 基本動作確認

### Step 2: Enhanced Features
- [ ] 複数キーボード同時表示
- [ ] キーボード自動識別
- [ ] 表示カスタマイズ機能
- [ ] OLEDサポート追加

### Step 3: Advanced Features
- [ ] バッテリー駆動対応
- [ ] Web設定インターフェース
- [ ] カスタムウィジェット対応
- [ ] アニメーション追加

## Technical Notes - 技術メモ

### BLE Advertisement制限事項
- データサイズ: 最大31バイト
- 送信間隔: 最短20ms（推奨1000ms）
- 一方向通信のみ

### 既存機能との互換性
- ドングルモードは維持（CONFIG_PROSPECTOR_MODE_DONGLE）
- スキャナーモードと排他的に動作
- 同じハードウェアで両方のモードをサポート

### 将来の拡張性
- Advertisement構造体の`reserved`フィールドで拡張可能
- プロトコルバージョンで後方互換性を維持
- 複数の通信方式を併用可能（Advertisement + GATT）