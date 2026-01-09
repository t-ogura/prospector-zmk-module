# Prospector Scanner 再構築設計仕様書

**作成日**: 2025-11-19
**バージョン**: v2.0-reconstruction
**基準タグ**: v2.0-milestone-2 (いつでも戻れる安定版)

---

## 1. 背景と目的

### 1.1 現状の問題

現在のアーキテクチャには以下の根本的な問題がある：

1. **BLEコールバックからLVGL直接呼び出し** - LVGLはシングルスレッドモデルなのに複数スレッドからアクセス
2. **Work QueueからLVGL呼び出し** - メインスレッド以外からのLVGL操作
3. **共有データの非保護アクセス** - `keyboards[]`配列、widgetポインタのrace condition
4. **Use-After-Free** - スワイプ中にwidget破棄、他スレッドがアクセス

これらはmutexで「症状を抑える」ことはできるが、根本解決にはならない。

### 1.2 目標

**フリーズしない処理フロー**を実現する：
- UI系はUI系、通信系は通信系でデータオーナーシップを明確化
- 割り込みはイベント/メッセージを投げるだけ
- 実際の処理・描画・状態更新はすべてメインタスクで実行
- LVGLは必ずメインタスクでのみ操作

---

## 2. アーキテクチャ設計

### 2.1 実行コンテキストの分離

```
┌─────────────────────────────────────────────────────────┐
│                    ISR / Callback層                      │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │
│  │ BLE Scan    │  │ Touch IRQ   │  │ Timer IRQ   │      │
│  │ Callback    │  │ Handler     │  │ (Battery)   │      │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘      │
│         │                │                │              │
│         │ k_msgq_put()   │ k_msgq_put()   │ k_msgq_put() │
│         │                │                │              │
└─────────┼────────────────┼────────────────┼──────────────┘
          │                │                │
          v                v                v
    ┌─────────────────────────────────────────────┐
    │            Message Queue (k_msgq)            │
    └──────────────────────┬──────────────────────┘
                           │
                           v
    ┌─────────────────────────────────────────────┐
    │              Main Task (UI Owner)           │
    │  ┌─────────────────────────────────────┐    │
    │  │  Message Loop                       │    │
    │  │  - k_msgq_get() でメッセージ受信    │    │
    │  │  - keyboards[] 状態更新             │    │
    │  │  - LVGL widget 更新                 │    │
    │  │  - 画面遷移処理                     │    │
    │  └─────────────────────────────────────┘    │
    └─────────────────────────────────────────────┘
```

### 2.2 データオーナーシップ

| データ | オーナー | 書き込み | 読み取り |
|--------|----------|----------|----------|
| `keyboards[]` 配列 | Main Task | Main Task のみ | Main Task のみ |
| Widget ポインタ | Main Task | Main Task のみ | Main Task のみ |
| LVGL オブジェクト | Main Task | Main Task のみ | Main Task のみ |
| スキャン状態フラグ | Main Task | Main Task のみ | Main Task のみ |
| 選択キーボードindex | Main Task | Main Task のみ | Main Task のみ |

**重要**: ISR/Callback層は一切これらのデータに直接アクセスしない。

### 2.3 メッセージ定義

```c
// メッセージ種別
enum scanner_msg_type {
    SCANNER_MSG_KEYBOARD_DATA,      // BLEからキーボードデータ受信
    SCANNER_MSG_KEYBOARD_TIMEOUT,   // キーボードタイムアウト検出
    SCANNER_MSG_SWIPE_GESTURE,      // スワイプジェスチャー検出
    SCANNER_MSG_BATTERY_UPDATE,     // バッテリー更新要求（定期）
    SCANNER_MSG_TOUCH_TAP,          // タップ検出（キーボード選択用）
};

// メッセージ構造体
struct scanner_message {
    enum scanner_msg_type type;
    uint32_t timestamp;             // k_uptime_get_32()

    union {
        // SCANNER_MSG_KEYBOARD_DATA
        struct {
            struct zmk_status_adv_data adv_data;
            int8_t rssi;
            char device_name[32];
        } keyboard;

        // SCANNER_MSG_SWIPE_GESTURE
        struct {
            enum swipe_direction direction;
        } swipe;

        // SCANNER_MSG_TOUCH_TAP
        struct {
            int16_t x;
            int16_t y;
        } tap;
    };
};

// メッセージキュー定義
K_MSGQ_DEFINE(scanner_msgq, sizeof(struct scanner_message), 16, 4);
```

---

## 3. 設計原則（厳守事項）

### 3.1 絶対禁止事項

1. **ISR/CallbackからLVGL APIを呼ばない**
   - `lv_label_set_text()`, `lv_obj_align()` 等すべて禁止
   - `lv_obj_create()`, `lv_obj_del()` も禁止

2. **ISR/Callbackから共有データに直接アクセスしない**
   - `keyboards[]` への読み書き禁止
   - widget ポインタの参照禁止

3. **Work Queueを使わない**
   - `K_WORK_DELAYABLE_DEFINE` を廃止
   - すべての定期処理はMain Taskのタイマーで実行

4. **mutexでLVGLスレッド安全性を確保しようとしない**
   - mutexはLVGLの問題を解決しない
   - そもそもmutexが不要な設計にする

### 3.2 推奨パターン

**ISR/Callbackでの処理:**
```c
static void scan_callback(const bt_addr_le_t *addr, int8_t rssi, ...) {
    // ❌ 禁止: keyboards[i] = data;
    // ❌ 禁止: lv_label_set_text(...);
    // ❌ 禁止: event_callback(&data);

    // ✅ 正解: メッセージをキューに入れるだけ
    struct scanner_message msg = {
        .type = SCANNER_MSG_KEYBOARD_DATA,
        .timestamp = k_uptime_get_32(),
    };
    memcpy(&msg.keyboard.adv_data, adv_data, sizeof(*adv_data));
    msg.keyboard.rssi = rssi;
    strncpy(msg.keyboard.device_name, name, sizeof(msg.keyboard.device_name));

    // 非ブロッキング送信（キューがいっぱいなら破棄）
    k_msgq_put(&scanner_msgq, &msg, K_NO_WAIT);
}
```

**Main Taskでの処理:**
```c
static void scanner_main_loop(void) {
    struct scanner_message msg;

    while (1) {
        // メッセージ待ち（タイムアウト付き）
        if (k_msgq_get(&scanner_msgq, &msg, K_MSEC(100)) == 0) {
            switch (msg.type) {
                case SCANNER_MSG_KEYBOARD_DATA:
                    // ここでkeyboards[]を更新
                    update_keyboard_from_message(&msg.keyboard);
                    // ここでLVGLを更新（安全）
                    update_display_widgets();
                    break;

                case SCANNER_MSG_SWIPE_GESTURE:
                    // スワイプ処理（widget破棄/作成）
                    handle_swipe(msg.swipe.direction);
                    break;

                // ... 他のメッセージ処理
            }
        }

        // 定期処理（タイムアウト時に実行）
        check_keyboard_timeouts();
        update_battery_display();
    }
}
```

---

## 4. 実装計画

### Phase 1: メッセージキュー基盤

1. `scanner_message.h` - メッセージ定義
2. `scanner_main.c` - メインタスクとメッセージループ
3. `status_scanner.c` 改修 - メッセージ送信のみに変更

### Phase 2: BLEスキャン処理の移行

1. `scan_callback()` をメッセージ送信のみに
2. `keyboards[]` の更新をMain Taskに移動
3. タイムアウト処理をMain Taskに移動

### Phase 3: タッチ/スワイプ処理の移行

1. 既存のZMKイベント → メッセージキューに統一
2. スワイプ処理をメインループに統合

### Phase 4: 定期処理の移行

1. Work Queue全廃
2. battery_debug, battery_periodic をメインループに統合
3. keyboard_list update をメインループに統合

### Phase 5: クリーンアップ

1. 不要なmutex削除
2. 不要なフラグ削除
3. テスト・最適化

---

## 5. 移行時の注意点

### 5.1 ZMKとの統合

現在ZMKの`zmk_display_status_screen()`がディスプレイ初期化を担当している。
メインループをどこに配置するか検討が必要：

**選択肢A**: ZMKのディスプレイタスク内に統合
- ZMKの`display/main.c`がLVGLタスクを管理
- この中でメッセージキューを処理

**選択肢B**: 独自スレッドを作成
- `K_THREAD_DEFINE`で専用スレッドを作成
- LVGLとの同期が必要

→ **選択肢A推奨**: ZMKのアーキテクチャに沿った形

### 5.2 既存機能の維持

以下の機能が移行後も動作することを確認：
- [ ] キーボード検出・表示
- [ ] バッテリー表示
- [ ] レイヤー表示
- [ ] モディファイア表示
- [ ] WPM表示
- [ ] スワイプによる画面遷移
- [ ] キーボード選択
- [ ] 設定画面（Bootloader/Reset）

### 5.3 パフォーマンス考慮

- メッセージキューサイズ: 16メッセージ（BLEが高頻度のため）
- メインループタイムアウト: 100ms（定期処理の粒度）
- BLE更新頻度: 変更なし（キーボード側が制御）

---

## 6. テスト計画

### 6.1 基本動作テスト

1. 単一キーボード検出・表示
2. 複数キーボード（5台）同時接続
3. キーボード切断・再接続
4. スワイプ操作（上下左右）
5. キーボード選択（タップ）

### 6.2 ストレステスト

1. 高頻度スワイプ連打
2. 複数キーボードからの同時更新
3. 長時間連続動作（1時間以上）
4. 画面遷移中のキーボード切断

### 6.3 成功基準

- **フリーズなし**: 上記テストすべてでフリーズしないこと
- **データ整合性**: 表示が常に最新の状態を反映すること
- **応答性**: スワイプ後200ms以内に画面遷移すること

---

## 7. 参考情報

### 7.1 Zephyr API

- `k_msgq_put()` / `k_msgq_get()` - メッセージキュー
- `k_sem_give()` / `k_sem_take()` - セマフォ
- `k_poll_signal_raise()` - ポールシグナル

### 7.2 関連ファイル

- `modules/prospector-zmk-module/src/status_scanner.c`
- `modules/prospector-zmk-module/boards/shields/prospector_scanner/src/scanner_display.c`
- `modules/prospector-zmk-module/boards/shields/prospector_scanner/src/keyboard_list_widget.c`
- `modules/prospector-zmk-module/boards/shields/prospector_scanner/src/touch_handler.c`

### 7.3 安定版への戻し方

```bash
cd zmk-config-prospector
git checkout v2.0-milestone-2

cd modules/prospector-zmk-module
git checkout v2.0-milestone-2
```

---

## 更新履歴

| 日付 | 内容 |
|------|------|
| 2025-11-19 | 初版作成 |

