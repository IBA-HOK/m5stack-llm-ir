# ハードウェア仕様と接続

仕様書 3 章（ハードウェア要件とピン仕様）の実装メモです。

## 対応ボードとビルド環境

| ボード | PlatformIO 環境 | Unit IR 接続先 Grove ポート | IR_TX（黄） | IR_RX（白） |
| --- | --- | --- | --- | --- |
| M5Stack CoreS3（推奨） | `m5stack-cores3` / `m5stack-cores3-ir` | PORT.B（黒） | GPIO9 | GPIO8 |
| M5Stack Core2 | `m5stack-core2` / `m5stack-core2-ir` | PORT.B（黒） | GPIO26 | GPIO36（入力専用） |
| M5StickC Plus2 | `m5stack-stickc-plus2` / `m5stack-stickc-plus2-ir` | Grove（PORT.CUSTOM） | GPIO32 | GPIO33 |
| M5Stack AtomS3 | `m5stack-atoms3` / `m5stack-atoms3-ir` | Grove（PORT.CUSTOM） | GPIO2 | GPIO1 |
| M5Stack Core（初代 Basic/Gray, 16MB） | `m5stack-core16m` / `m5stack-core16m-ir` | **本体に黒 Grove なし**: M5-Bus へ配線 | GPIO26 | GPIO36 |

* `-ir` 付きの環境は **Unit IR 接続後**に使用します（`M5IR_ENABLE_IR_HW=1`）。
* それ以外の環境はスタブ IR バックエンドでビルドされ、IR 系 API は `503 IR_NOT_AVAILABLE` を返します。
* M5Stack Basic / Gray の Port A は I2C 専用のため、Unit IR をそのまま接続できません
  （仕様 3.2）。Port B を持つ機種・ベース拡張、または GPIO 兼用 Port A を持つ Core2 /
  M5StickC / ATOM 系を使用してください。
* ピン番号はコードへ直書きせず、`firmware/platformio.ini` の
  `M5IR_IR_TX_PIN` / `M5IR_IR_RX_PIN` ビルドフラグで機種別に差し替えます。

## Unit IR (U002) の Grove ピンアサイン

M5Stack 公式ドキュメント（Unit IR）の PinMap:

| HY2.0-4P | 黒 | 赤 | 黄 | 白 |
| --- | --- | --- | --- | --- |
| PORT.B | GND | 5V | **IR_TX** | **IR_RX** |

* 黄色線（Grove 内側）= 送信 LED（38 kHz）
* 白色線（Grove 外側）= 受光センサー
* 仕様 3.2 の「送信ピン（内側・黄色）/ 受信ピン（外側・白色）」と一致します。

> **注意**: Unit IR は Grove の 5V と GND を使います。CoreS3 / Core2 / M5StickC Plus2 /
> AtomS3 の Grove ポートはいずれも 5V を供給するので、そのまま接続できます。

### 赤い Grove（PORT.A）で使う（実験的サポート）

初代 Core の赤い Grove PORT.A は通常 I2C（G21/G22）ですが、ファームウェア側で内部 I2C を
解放すれば Unit IR 用の GPIO として使えます（**実験的**）。配線は Grove ケーブルをそのまま
PORT.A へ接続するだけです。

| PORT.A (赤) | 信号 |
| --- | --- |
| 黒 | GND |
| 赤 | 5V |
| 黄（内側） | **IR_TX = G21** |
| 白（外側） | **IR_RX = G22** |

```bash
# PORT.A 版（内蔵 I2C を解放して G21/G22 を IR に使用）
tools/flash.sh m5stack-core16m-porta-ir
```

* 起動ログに `[ir] internal I2C released; PORT.A G21/G22 are free for IR` が出れば成功です。
* **実機での結果（2026-11-27, U002）**: 送信 LED は G21 で点滅を確認。受信は G22 で
  リモコン信号を検出できましたが、Port A は I2C 用プルアップが残るため**受信波形が
  ノイズ混じりになりやすく**、AC の生波形再送は不安定でした。安定運用には M5-Bus の
  G26/G36（Port B 相当）への配線を推奨します。
* Unit IR の送信 LED は回路図上 R1=150Ω で約 25mA 駆動（一般的なリモコンは 100mA 超）。
  「フルパワー」にするには R1 の交換（例: 47Ω で約 75mA）や外部 IR LED ドライバが必要です。
  ソフトウェア（IRsend の duty 50%・2 回送信）だけでは電流は増やせません。
* 内蔵 I2C（IP5306 のバッテリー情報など）はこのビルドでは使えません（USB 給電前提）。
* PORT.A には I2C 用のプルアップ抵抗が残るため、信号品質は Port B / M5-Bus 接続より
  有利とは限りません。学習・送信の実機確認を必ず行ってください。
* 仕様書 3.2 は Port A の直接接続を想定していません（本対応は実験的な拡張です）。

### 初代 M5Stack Core（Basic / Gray）への接続

初代 Core の本体 Grove は **赤い PORT.A（I2C: G21/G22）のみ** で、黒い PORT.B は
本体に実装されていません（仕様 3.2 の注意点）。Unit IR はそのまま挿せないため、
次のいずれかで接続します。

1. **M5-Bus へジャンパ配線**（推奨・追加部品なし）:
   - Unit IR の付属 Grove ケーブルを切り出し、黄線 → **G26**（M5-Bus RIGHT 10 番, DAC）、
     白線 → **G36**（M5-Bus RIGHT 4 番, ADC）、黒 → GND、赤 → 5V。
   - `firmware/platformio.ini` の `m5stack-core16m` は TX=26 / RX=36 を既定にしています。
2. **Port B（黒 Grove）付きベース拡張**を使い、同じ G26/G36 を配線する。
3. 将来 Core2 / CoreS3 / M5StickC Plus2 / AtomS3 に載せ替える場合は、それぞれの
   `-ir` 環境へ切り替えるだけでピン設定も自動で切り替わります。

> 初代 Core の実機は `ESP32-D0WDQ6-V3 / 16MB Flash / 320x240 ILI9342C`、
> M5GFX のボード検出は `board=1 (board_M5Stack)` でした（本リポジトリで確認済み）。

## ソフトウェア側の IR 構成

`firmware/src/ir_engine.cpp`:

* `M5IR_ENABLE_IR_HW=0`（既定）: スタブ。`/api/health` の `ir.hardware=false`、
  IR 系 API は `503 IR_NOT_AVAILABLE`。
* `M5IR_FAKE_IR_BACKEND=1`（`-test` 環境）: 合成 IR バックエンド。Unit IR 未接続でも
  `learn` → `test` → 保存 → 送信 → 削除の API ライフサイクルを実機で試験できる
  （`/api/health` の `ir.backend` が `fake`）。**本番使用禁止**。
* `M5IR_ENABLE_IR_HW=1`: IRremoteESP8266 2.9.0 を使用（`ir.backend` は `irremote`）。
  * 受信: `IRrecv`（バッファ 1024 tick、tolerance 25%）。
  * 送信: `IRsend`（38 kHz、`sendNEC` / `sendSony` / `sendSAMSUNG` / `sendRC5` /
    `sendRC6` / `sendPanasonic64` / `sendJVC` / `sendLG` / `sendDISH` / `sendSharpRaw` /
    `sendMitsubishi` / `sendMitsubishi2` / `sendCOOLIX` / `sendWhynter` /
    `sendAiwaRCT501` / `sendMagiQuest` / `sendRCMM` / `sendSanyoLC7461`）。
  * それ以外の既知プロトコルと未知プロトコルは、生の µs 配列（`format: "raw"`）として
    保存し `sendRaw` で再送します。IRrecv の rawbuf は 2 µs tick のため
    `rawbuf[i] * kRawTick` で µs に変換しています（先頭の gap エントリはスキップ）。

### 学習データの形式

`POST /api/ir/learn` のレスポンスは仕様 5.3 に従い、生波形を返さず
`capture_id` / `protocol` / `bits` / `raw_length` のみを返します。
キャプチャは RAM 上に最新 3 件・10 分間保持されます（仕様 5.3）。

## USB・書き込み

| ボード | USB シリアル | Linux のデバイス例 | 備考 |
| --- | --- | --- | --- |
| CoreS3 / AtomS3 | ESP32-S3 内蔵 USB（OTG/CDC） | `/dev/ttyACM0` | 追加ドライバ不要 |
| Core2 | CP2104 または CH9102F | `/dev/ttyUSB0` | `cp210x` / `ch341` カーネルモジュール |
| M5StickC Plus2 | CH9102F | `/dev/ttyUSB0` | `ch341`（CH343 系）ドライバ |

Linux で書き込み権限がない場合は `dialout` グループへ追加してください:

```bash
sudo usermod -aG dialout "$USER"   # 再ログインが必要
```

PlatformIO での書き込み例:

```bash
cd firmware
../.venv/bin/pio run -e m5stack-cores3-ir -t upload
../.venv/bin/pio device list        # ポート一覧
../.venv/bin/pio device monitor     # 115200 baud
```

シリアルコンソールには起動ログ（Wi-Fi / IP / IR バックエンド / relay 状態）が出力されます。

## 筐体・設置メモ

* Unit IR の有効距離は ≤ 5 m です。設置時は赤外線 LED を家電の受光部へ向けてください。
* Grove ケーブルは付属の HY2.0-4P を使用します（ピンアサインを上表で確認）。
* 学習中は LCD に「Press remote! 残り秒数」が表示されます（仕様 8 章）。
