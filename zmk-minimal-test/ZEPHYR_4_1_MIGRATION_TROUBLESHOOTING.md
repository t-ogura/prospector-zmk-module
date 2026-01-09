# Zephyr 4.1 移行トラブルシューティングガイド

**作成日**: 2025-12-17
**対象**: ZMK v0.3.0 (Zephyr 3.5) → ZMK main (Zephyr 4.1) 移行
**ハードウェア**: Seeed XIAO nRF52840 + Waveshare 1.69" Round LCD (ST7789V)

---

## 概要

Prospector ScannerモジュールをZephyr 4.1に移行する際に遭遇した問題と解決策をまとめたドキュメントです。

### 環境変更

| 項目 | 移行前 | 移行後 |
|------|--------|--------|
| ZMK | v0.3.0 | main branch |
| Zephyr | 3.5 | 4.1 |
| LVGL | 8.x | 9.3 |
| ボード名 | `seeeduino_xiao_ble` | `xiao_ble` |

---

## 問題1: ボード名の変更

### 症状
```
ERROR: Board 'seeeduino_xiao_ble' not found
```

### 原因
Zephyr 4.1でボード名が変更された。

### 解決策
ビルドコマンドのボード名を変更：
```bash
# 旧
west build -b seeeduino_xiao_ble ...

# 新
west build -b xiao_ble ...
```

---

## 問題2: ST7789Vドライバーの構造変更

### 症状
- SPIログは正常（134400バイト転送成功）
- しかし画面は真っ黒

### 原因
Zephyr 4.1ではST7789VドライバーがMIPI DBIインターフェース経由に変更された。旧スタイルの直接SPI接続は動作しない。

### 解決策

**旧スタイル (Zephyr 3.5)**:
```dts
&spi3 {
    st7789v: st7789v@0 {
        compatible = "sitronix,st7789v";
        spi-max-frequency = <31000000>;
        reg = <0>;
        cmd-data-gpios = <&xiao_d 7 GPIO_ACTIVE_LOW>;
        reset-gpios = <&xiao_d 3 GPIO_ACTIVE_LOW>;
        width = <240>;
        height = <280>;
        /* ... other properties ... */
    };
};
```

**新スタイル (Zephyr 4.1)**:
```dts
/* MIPI DBIラッパーが必須 */
/ {
    mipi_dbi {
        compatible = "zephyr,mipi-dbi-spi";
        spi-dev = <&spi3>;
        dc-gpios = <&xiao_d 7 GPIO_ACTIVE_HIGH>;  /* 極性に注意! */
        reset-gpios = <&xiao_d 3 GPIO_ACTIVE_LOW>;
        write-only;
        #address-cells = <1>;
        #size-cells = <0>;

        st7789v: st7789v@0 {
            compatible = "sitronix,st7789v";
            reg = <0>;
            mipi-max-frequency = <20000000>;
            mipi-mode = "MIPI_DBI_MODE_SPI_4WIRE";
            width = <240>;
            height = <280>;
            /* ... other properties ... */
        };
    };
};
```

### 重要: DCピンの極性

| ドライバー | プロパティ名 | 極性 |
|-----------|-------------|------|
| 旧 (直接SPI) | `cmd-data-gpios` | `GPIO_ACTIVE_LOW` |
| 新 (MIPI DBI) | `dc-gpios` | `GPIO_ACTIVE_HIGH` |

**理由**: MIPI DBIドライバーは以下のロジックを使用：
- `gpio_pin_set_dt(0)` → コマンドモード → 電気的LOW
- `gpio_pin_set_dt(1)` → データモード → 電気的HIGH

`GPIO_ACTIVE_HIGH`で正しい電気信号が生成される。

---

## 問題3: 画面が真っ黒（blanking_offが呼ばれない）

### 症状
- SPIログは正常
- バックライトは点灯
- しかし画面は真っ黒

### 原因
ZMKの`initialize_display()`関数で、`zmk_display_status_screen()`がNULLを返すと`display_blanking_off()`が呼ばれない。

```c
// zmk/app/src/display/main.c
screen = zmk_display_status_screen();
if (screen == NULL) {
    LOG_ERR("No status screen provided");
    return;  // ← blanking_offが呼ばれない!
}
lv_scr_load(screen);
unblank_display_cb(work);  // ← ここでblanking_offが呼ばれる
```

### 解決策
以下のいずれかを設定：
```conf
# ビルトインステータス画面を使用
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_BUILT_IN=y

# または、カスタムステータス画面を使用
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y
```

---

## 問題4: lv_label_create()でフリーズ

### 症状
- `lv_obj_create(NULL)` は成功
- `lv_label_create(screen)` でフリーズ
- ログも見れなくなる（USB接続断）

### 原因
`CONFIG_LV_Z_MEM_POOL_SIZE`がデフォルトで2048バイトと小さすぎる。ラベル作成時のメモリ確保に失敗してクラッシュする。

### 解決策
メモリプールサイズを増加：
```conf
# カスタム画面使用時は必須
CONFIG_LV_Z_MEM_POOL_SIZE=4096

# より複雑なUIの場合はさらに増加
# CONFIG_LV_Z_MEM_POOL_SIZE=8192
```

### 調査方法
ビルトイン画面とカスタム画面のKconfig差分を比較：
```bash
# カスタム画面でビルド → config保存
cp build/zephyr/.config /tmp/config_custom.txt

# ビルトイン画面でビルド → config保存
cp build/zephyr/.config /tmp/config_builtin.txt

# 差分確認
diff /tmp/config_custom.txt /tmp/config_builtin.txt | grep LV
```

---

## 問題5: 色反転プロパティの変更

### 症状
```
error: 'inversion-on' is not a valid property
```

### 原因
Zephyr 4.1のST7789Vバインディングでは`inversion-on`プロパティが削除され、代わりに`inversion-off`が追加された。

### 解決策
```dts
/* Zephyr 3.5 */
inversion-on;  /* 色反転を有効化 */

/* Zephyr 4.1 */
/* デフォルトで色反転ON */
/* 無効化したい場合のみ: */
/* inversion-off; */
```

---

## 最終的な動作設定

### Kconfig (display_test.conf)
```conf
# Display
CONFIG_ZMK_DISPLAY=y
CONFIG_DISPLAY=y
CONFIG_LVGL=y

# Status screen
CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM=y

# LVGL memory pool (重要!)
CONFIG_LV_Z_MEM_POOL_SIZE=4096

# Font
CONFIG_LV_FONT_MONTSERRAT_16=y
CONFIG_LV_USE_LABEL=y

# Disable BLE for testing
CONFIG_ZMK_BLE=n
CONFIG_BT=n

# USB Logging
CONFIG_LOG=y
CONFIG_ZMK_USB_LOGGING=y
```

### Overlay (display_test.overlay)
```dts
#include <dt-bindings/zmk/matrix_transform.h>

/ {
    chosen {
        zmk,kscan = &kscan0;
        zmk,display = &st7789v;
        zephyr,display = &st7789v;
        zephyr,console = &cdc_acm_uart;
    };

    kscan0: kscan {
        compatible = "zmk,kscan-gpio-direct";
        wakeup-source;
        input-gpios = <&xiao_d 0 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
    };

    leds {
        compatible = "gpio-leds";
        backlight: backlight {
            gpios = <&gpio1 11 GPIO_ACTIVE_HIGH>;
            label = "Backlight";
        };
    };

    aliases {
        led0 = &backlight;
    };
};

&zephyr_udc0 {
    cdc_acm_uart: cdc_acm_uart {
        compatible = "zephyr,cdc-acm-uart";
    };
};

&spi2 {
    status = "disabled";
};

&pinctrl {
    spi3_default: spi3_default {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 1, 13)>,
                <NRF_PSEL(SPIM_MOSI, 1, 15)>,
                <NRF_PSEL(SPIM_MISO, 1, 10)>;
        };
    };

    spi3_sleep: spi3_sleep {
        group1 {
            psels = <NRF_PSEL(SPIM_SCK, 1, 13)>,
                <NRF_PSEL(SPIM_MOSI, 1, 15)>,
                <NRF_PSEL(SPIM_MISO, 1, 10)>;
            low-power-enable;
        };
    };
};

&spi3 {
    status = "okay";
    pinctrl-0 = <&spi3_default>;
    pinctrl-1 = <&spi3_sleep>;
    pinctrl-names = "default", "sleep";
    cs-gpios = <&xiao_d 9 GPIO_ACTIVE_LOW>;
};

/ {
    mipi_dbi {
        compatible = "zephyr,mipi-dbi-spi";
        spi-dev = <&spi3>;
        dc-gpios = <&xiao_d 7 GPIO_ACTIVE_HIGH>;
        reset-gpios = <&xiao_d 3 GPIO_ACTIVE_LOW>;
        write-only;
        #address-cells = <1>;
        #size-cells = <0>;

        st7789v: st7789v@0 {
            compatible = "sitronix,st7789v";
            reg = <0>;
            mipi-max-frequency = <20000000>;
            mipi-mode = "MIPI_DBI_MODE_SPI_4WIRE";
            width = <240>;
            height = <280>;
            x-offset = <0>;
            y-offset = <20>;
            vcom = <0x19>;
            gctrl = <0x35>;
            vrhs = <0x12>;
            vdvs = <0x20>;
            mdac = <0x00>;
            gamma = <0x01>;
            colmod = <0x05>;
            lcm = <0x2c>;
            porch-param = [0c 0c 00 33 33];
            cmd2en-param = [5a 69 02 01];
            pwctrl1-param = [a4 a1];
            pvgam-param = [D0 04 0D 11 13 2B 3F 54 4C 18 0D 0B 1F 23];
            nvgam-param = [D0 04 0C 11 13 2C 3F 44 51 2F 1F 1F 20 23];
            ram-param = [00 F0];
            rgb-param = [CD 08 14];
        };
    };
};
```

---

## デバッグのヒント

### 1. ハートビートタイマーでデバイス生存確認
```c
static void heartbeat_timer_cb(struct k_timer *timer) {
    LOG_INF("Heartbeat - device alive");
}
K_TIMER_DEFINE(heartbeat_timer, heartbeat_timer_cb, NULL);

// 初期化時に開始
k_timer_start(&heartbeat_timer, K_SECONDS(3), K_SECONDS(3));
```

### 2. 段階的な機能追加
フリーズの原因特定のため、一度に一つの機能のみ追加：
1. 画面作成のみ → 動作確認
2. 背景色設定 → 動作確認
3. ラベル作成 → 動作確認
4. テキスト設定 → 動作確認

### 3. Kconfig差分の比較
動作するビルドと動作しないビルドの`.config`を比較して差分を特定。

---

## チェックリスト

Zephyr 4.1移行時に確認すべき項目：

- [ ] ボード名を`xiao_ble`に変更
- [ ] ST7789VをMIPI DBIラッパーで囲む
- [ ] `dc-gpios`を`GPIO_ACTIVE_HIGH`に設定
- [ ] `CONFIG_LV_Z_MEM_POOL_SIZE=4096`以上を設定
- [ ] ステータス画面を有効化（BUILT_INまたはCUSTOM）
- [ ] 使用するフォントを有効化
- [ ] `inversion-on`を削除（デフォルトでON）

---

## 参考リンク

- [ZMK Zephyr 4.1 Update Blog](https://zmk.dev/blog/2025/12/09/zephyr-4-1)
- [Zephyr ST7789V Binding](https://docs.zephyrproject.org/latest/build/dts/api/bindings/display/sitronix,st7789v.html)
- [LVGL 9 Migration Guide](https://docs.lvgl.io/master/intro/migration.html)
