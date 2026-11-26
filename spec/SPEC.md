<!--
  このファイルは共有された仕様書（Gemini Canvas）のローカルコピーです。
  取得元: https://share.gemini.google/QzSq4mluA4t0
  取得方法: headless Chromium で描画した DOM の div.ProseMirror を Markdown へ変換
  変換: spec/extract_spec.py
-->

# M5Stack LLM スマートリモコン（MVP）仕様書

**バージョン:** 0.2.0-draft

**最終更新日:** 2026-11-25

**対象プロジェクト:** 宅外LANからLLMエージェントがHTTP API経由で操作できるオープンソース（OSS）スマートリモコン

## 1. プロジェクト概要とMVPスコープ

本プロジェクトは、M5Stackと[M5Stack Unit IR (U002)](https://docs.m5stack.com/ja/unit/Unit_IR)を使い、宅外LAN（外出先・VPS・クラウド）上で動作するLLMエージェント（[Hermes Agent](https://github.com/nousresearch/hermes-agent)やMuse等）からHTTP API経由で赤外線家電を操作・学習・管理できるOSSスマートリモコンの最小構成（MVP）を定義します。

### 1.1 MVPに含める範囲（In Scope）

- **赤外線リモコンの動的ライフサイクル管理:** 登録済み家電の一覧取得、リモコン信号の学習、テスト送信、永続保存（LittleFS）、単発および連続バッチ送信、削除。
- **HTTP REST API（ローカルLAN待受）:** 外部アカウント不要で即座に動作する軽量HTTPサーバーと `Authorization: Bearer` 認証。
- **M5Stack単体向け無料HTTPS逆トンネル中継（オプション）:** [Cloudflare Workers Freeプラン](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fworkers%2Fplatform%2Fpricing%2F)のSQLiteバックエンド[Durable Objects](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fchangelog%2Fpost%2F2025-04-07-durable-objects-free-tier%2F)を使い、自宅に常時稼働PCやVPSを置かずに宅外から同一のHTTP APIへアクセスできるWebSocket逆トンネルリレー（`relay/`）。
- **自宅PC経由の宅外アクセス手順:** [Tailscale](https://tailscale.com/pricing)（Subnet Router）または[Cloudflare Tunnel](https://www.cloudflare.com/plans/free/)を使った接続ガイド。
- **エージェント向けスキル仕様:** 自然言語から対話的に家電登録・操作・マクロ実行を行うための手順書（`SKILL.md`）。
- **M5Stack LCD フィードバック:** IPアドレス、リレー接続状態、待機状態、赤外線学習カウントダウン、送受信結果の画面表示。

### 1.2 MVPに含めない範囲（Out of Scope / Phase 2以降）

- MQTTブローカー連携。
- M5Stack本体側でのマクロ（シーン）保存・スケジュール実行（MVPではエージェント側のスキル／メモリへ委譲）。
- 赤外線以外のM5 Unit（温湿度・人感・リレー等）のドライバ実装。
- Home Assistant本体向けカスタム統合（[Marketplace / 旧HACS](https://www.hacs.xyz/docs/marketplace/)配布用コンポーネント）。ただし将来の統合を見据え、エンティティ指向の簡潔なREST設計を採用します。

## 2. システムアーキテクチャと責務分離

ハードウェア層・中継層・知能層の責務を明確に分離し、M5Stackのファームウェアを薄く堅牢に保ちます。また、M5Stack内部で「APIハンドラ（コマンド実行層）」を通信層から分離することで、ローカルHTTPとWebSocket逆トンネルのどちらから届いたリクエストも同一ロジックで処理します。

| レイヤー | コンポーネント | 担当する責務 |
|---|---|---|
| **知能層（宅外／クラウド）** | LLMエージェント（[Hermes Agent](https://hermes-agent.nousresearch.com/docs/) / Muse等） | 自然言語の解釈、対話による家電学習フローの進行、マクロ・シーン管理、API呼び出し |
| **ネットワーク中継層（宅外用）** | [Cloudflare Workers](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fworkers%2Fplatform%2Fpricing%2F)＋[Durable Objects](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fdurable-objects%2F)（M5単体向け）、または [Tailscale](https://tailscale.com/docs/reference/devices-without-tailscale) / [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/account-limits/)（自宅PCあり向け） | 宅外からのHTTPS終端・認証およびM5Stackへの安全なリクエスト転送 |
| **ハードウェア制御層** | M5Stack本体（ESP32 / ESP32-S3） | ローカルHTTP待受、任意WSS逆トンネル接続、Bearer認証、LittleFSへのIRデータ永続化、LCD表示、排他制御 |
| **物理I/O層** | [Unit IR (U002)](https://docs.m5stack.com/ja/unit/Unit_IR) | 38kHz赤外線LED送信および赤外線リモコン信号の受光 |

## 3. ハードウェア要件とピン仕様

### 3.1 対応ハードウェア

- **マイコン本体:** Wi-Fi対応のM5Stackシリーズ（推奨: [M5Stack CoreS3](https://docs.m5stack.com/ja/core/CoreS3) / CoreS3-SE / Core2 / M5StickC Plus2 / ATOM S3等）。
- **赤外線ユニット:** [M5Stack Unit IR (U002)](https://docs.m5stack.com/ja/unit/Unit_IR)（送信LED・受光センサー一体型、黒Groveコネクタ）。
- **赤外線ライブラリ:** [IRremoteESP8266](https://github.com/crankyoldgit/irremoteesp8266)。

### 3.2 接続ポートとGPIO設定の注意点

- M5Stack Basic / GrayのPort AはI2C専用のためUnit IRをそのまま接続できません([Lang-ship](https://lang-ship.com/blog/work/m5stack-ir-unit/))。Port B（黒）を持つ機種・ベース拡張、またはGPIO兼用Port Aを持つCore2・M5StickC・ATOM系を使用してください。
- 送信ピン（Grove内側・黄色線）と受信ピン（Grove外側・白色線）のGPIO番号はコードへ直書きせず、`platformio.ini` のビルドフラグ（`IR_TX_PIN`, `IR_RX_PIN`）で機種別に差し替え可能にします。

## 4. ネットワーク・宅外アクセス・セキュリティ仕様

### 4.1 認証とアクセス制御

- M5Stackは自宅LAN内でHTTP（デフォルトポート `80`）を待ち受け、mDNS（例: `http://m5ir.local`）および固定ローカルIPで到達可能にします。
- `GET /api/health` を除くすべてのエンドポイントで、`Authorization: Bearer <API_KEY>` ヘッダーを必須とします。トークン不一致時は `401 Unauthorized` を返します。
- APIキーは32文字以上のランダム文字列を推奨し、ソースコードへコミットしない運用（`secrets.h` の `.gitignore` 指定、またはNVS保存）を必須とします。

### 4.2 宅外LANからの推奨接続方式

ルーターのポート開放は行わず、自宅環境に合わせて以下の3方式から選択します。いずれの方式でもエージェント側のAPIパスとJSON仕様は完全に同一です。

| 接続方式 | 自宅に常時稼働PCが必要か | 推奨ユースケース | セキュリティ・無料枠の特徴 |
|---|---|---|---|
| [**Cloudflare Workers**](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fworkers%2Fplatform%2Fpricing%2F)**＋**[**Durable Objects**](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fdurable-objects%2Fplatform%2Fpricing%2F)**（WebSocket逆トンネル）** | **不要（M5Stack単体）** | 自宅サーバーもVPSも置かず、M5Stackだけで宅外からHTTPS操作したい場合 | Workers FreeプランのSQLite Durable Objects枠（1日10万リクエスト上限）で無料運用可能。WSS外向き接続＋Bearer認証 |
| [**Tailscale**](https://tailscale.com/pricing)**（Subnet Router）** | 必要（ラズパイ等） | 自分のVPSや外出先PC上のエージェントから操作する場合 | APIをインターネットへ一切公開せず、閉域VPN経由でM5StackのローカルIPへ直接到達できる([公式手順](https://tailscale.com/docs/reference/devices-without-tailscale)) |
| [**Cloudflare Tunnel**](https://www.cloudflare.com/plans/free/) | 必要（`cloudflared`） | 既に自宅サーバーがあり、既存構成に載せたい場合 | ルーター開放不要でHTTPS終端でき、Cloudflare AccessやBearerトークンで二重防御できる |

### 4.3 誤作動・DoS防止ガードレール

- `POST /api/ir/learn` の `timeout_ms` 上限は `30000`（30秒）に制限します。
- `POST /api/ir/send` のバッチ `steps` 上限は `20` ステップ、1ステップあたりの `delay_ms` 上限は `5000`（5秒）に制限します。

## 5. HTTP API 仕様（v1）

### 5.1 共通規約

- **Content-Type:** `application/json; charset=utf-8`
- **成功レスポンス:** `{"ok": true, ...}`
- **エラーレスポンス:** `{"ok": false, "error": {"code": "<ERROR_CODE>", "message": "<詳細>"}}`
- **主なステータスコード:** `200 OK`, `201 Created`, `400 Bad Request`, `401 Unauthorized`, `404 Not Found`, `408 Request Timeout`（学習タイムアウト）, `409 Conflict`（学習中などビジー状態）, `503 Service Unavailable`（リレー経由時にM5Stack未接続）, `504 Gateway Timeout`（リレー応答タイムアウト）。

### 5.2 エンドポイント一覧

| メソッド | パス | 認証 | 概要 |
|---|---|---|---|
| `GET` | `/api/health` | 任意 | 稼働確認、Wi-Fi状態、空きフラッシュ容量、現在状態（`idle` / `learning`）を返す |
| `GET` | `/api/devices` | 必須 | 登録済みの全家電・アクション・別名一覧を返す |
| `POST` | `/api/ir/learn` | 必須 | 指定時間だけ赤外線を受光待機し、取得信号を一時保存して `capture_id` を返す |
| `POST` | `/api/ir/test` | 必須 | 一時保存中の `capture_id` をテスト送信して動作確認する |
| `POST` | `/api/devices/{device_id}/actions` | 必須 | `capture_id` を指定の家電・アクション名でLittleFSへ永続保存（新規作成・上書き） |
| `POST` | `/api/ir/send` | 必須 | 登録済みの単発アクション、または複数ステップの連続バッチを送信する |
| `DELETE` | `/api/devices/{device_id}` | 必須 | 指定した家電と配下の全アクションを削除する |
| `DELETE` | `/api/devices/{device_id}/actions/{action}` | 必須 | 指定した家電の特定アクションのみを削除する |

### 5.3 主要エンドポイントのリクエスト／レスポンス例

#### `GET /api/devices`

```
{
  "ok": true,
  "devices": {
    "living_tv": {
      "label": "リビングのテレビ",
      "actions": ["power", "vol_up", "vol_down", "mute"]
    },
    "living_light": {
      "label": "リビング照明",
      "actions": ["on", "off", "night"]
    }
  }
}
```

#### `POST /api/ir/learn`

LLMのコンテキスト浪費とJSON破損を防ぐため、生波形配列はレスポンスに含めず短い `capture_id` のみを返します。一時バッファは最新1〜3件をRAM上に保持し、一定時間（例: 10分）または再起動で破棄します。

- **Request:**

```
{
  "timeout_ms": 10000
}
```

- **Response (`200 OK`):**

```
{
  "ok": true,
  "capture_id": "cap_8f2a",
  "protocol": "NEC",
  "bits": 32,
  "raw_length": 67
}
```

#### `POST /api/ir/test`

- **Request:**

```
{
  "capture_id": "cap_8f2a"
}
```

- **Response (`200 OK`):**

```
{
  "ok": true,
  "sent": true,
  "capture_id": "cap_8f2a"
}
```

#### `POST /api/devices/{device_id}/actions`

- **Request（`device_id` は英小文字・数字・アンダースコアのみ許可）:**

```
{
  "label": "リビングのテレビ",
  "action": "power",
  "capture_id": "cap_8f2a"
}
```

- **Response (`201 Created`):**

```
{
  "ok": true,
  "device_id": "living_tv",
  "action": "power",
  "stored_format": "protocol"
}
```

#### `POST /api/ir/send`

単発指定と配列バッチ指定（`steps`）の両方を受け付けます。

- **Request（単発）:**

```
{
  "device": "living_light",
  "action": "off"
}
```

- **Request（連続バッチ送信）:**

```
{
  "steps": [
    { "device": "living_tv", "action": "power" },
    { "delay_ms": 400 },
    { "device": "living_light", "action": "off" }
  ]
}
```

- **Response (`200 OK`):**

```
{
  "ok": true,
  "executed_steps": 3
}
```

## 6. Cloudflare Workers 逆トンネル中継仕様（M5Stack単体向けオプション）

自宅に常時稼働PCを置かずに宅外からHTTPS接続するため、オプションの中継リレー（`relay/`）を同梱します。[Cloudflare Workers Freeプラン](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fworkers%2Fplatform%2Fpricing%2F)で利用可能なSQLiteバックエンドの[Durable Objects](https://www.google.com/search?q=https%3A%2F%2Fdevelopers.cloudflare.com%2Fchangelog%2Fpost%2F2025-04-07-durable-objects-free-tier%2F)（WebSocket Hibernation API）を前提とし、追加費用ゼロで運用できます。

### 6.1 接続とリクエスト中継の流れ

1. M5Stackの設定に `RELAY_WS_URL`（例: `wss://<worker-subdomain>.workers.dev/ws/device`）が設定されている場合、M5StackはWi-Fi接続後に `Authorization: Bearer <DEVICE_TOKEN>` ヘッダー付きで外向きWebSocket接続を確立する。
2. Durable Objectは単一のアクティブなM5Stackソケットを保持する。新規接続が届いた場合は既存の古いソケットを切断して差し替え、再起動直後のゴースト接続を防ぐ。
3. 宅外のLLMエージェントが `https://<worker-subdomain>.workers.dev/api/...` へ通常のHTTPリクエスト（`Authorization: Bearer <API_KEY>`）を送る。
4. Workerは `API_KEY` を検証したうえでDurable Objectへ転送し、Durable Objectが `req_id` を採番してWebSocketフレームとしてM5Stackへ送る。
5. M5StackはローカルHTTPと同じ内部ハンドラで処理を実行し、`req_id` 付きの応答フレームをWebSocketで返す。Durable Objectは対応するHTTPリクエストへステータスコードとJSONボディをそのまま返却する。

### 6.2 WebSocket JSONフレーム仕様

#### Relay → M5Stack（リクエスト転送）

```
{
  "type": "http_request",
  "req_id": "req_01J9X",
  "method": "POST",
  "path": "/api/ir/send",
  "body": {
    "device": "living_light",
    "action": "off"
  }
}
```

#### M5Stack → Relay（レスポンス返却）

```
{
  "type": "http_response",
  "req_id": "req_01J9X",
  "status": 200,
  "body": {
    "ok": true,
    "executed_steps": 1
  }
}
```

#### Keep-Alive と自動再接続

- M5Stackは30秒間隔で軽量な `{"type": "ping"}`（またはWebSocket制御フレームのPing）を送信し、Relayは `{"type": "pong"}` を返す。
- 応答が途絶えた場合またはソケット切断を検知した場合、M5Stackは指数バックオフ（2秒 → 4秒 → 8秒 → 最大30秒）で自動再接続する。

### 6.3 タイムアウト・オフライン時のエラー処理

| 異常ケース | HTTPステータス | エラーコード | 振る舞い |
|---|---|---|---|
| M5StackがWebSocket未接続 | `503 Service Unavailable` | `DEVICE_OFFLINE` | 待機せず即座にエラーJSONを返す |
| 通常APIでM5Stack応答なし | `504 Gateway Timeout` | `DEVICE_TIMEOUT` | 10秒でタイムアウトし保留中の `req_id` を破棄 |
| `/api/ir/learn` で応答なし | `504 Gateway Timeout` | `LEARN_TIMEOUT` | `timeout_ms + 5000ms`（最大35秒）でタイムアウト |
| `API_KEY` / `DEVICE_TOKEN` 不一致 | `401 Unauthorized` | `UNAUTHORIZED` | Worker入口で即座に拒否しDurable Objectへ渡さない |

### 6.4 認証トークンの分離

- エージェントがHTTPリクエストに付ける `API_KEY` と、M5StackがWebSocket接続に使う `DEVICE_TOKEN` は別々のシークレット（`wrangler secret put` で登録）として管理します。
- これにより、エージェント側のAPIキーをローテーションしてもM5Stackの再書き込みが不要になります。

## 7. データモデルと永続化仕様（LittleFS）

### 7.1 保存ファイル構造（`/ir_db.json`）

[IRremoteESP8266](https://github.com/crankyoldgit/irremoteesp8266)で既知プロトコル（NEC、SONY、PANASONIC等）と判定できた信号は `format: "protocol"` として軽量保存し、未知プロトコル（`UNKNOWN`）の場合は `format: "raw"` としてマイクロ秒配列（`raw_data`）を保存します。

```
{
  "schema_version": 1,
  "devices": {
    "living_tv": {
      "label": "リビングのテレビ",
      "actions": {
        "power": {
          "format": "protocol",
          "protocol": "NEC",
          "code": "0x20DF10EF",
          "bits": 32
        }
      }
    },
    "custom_fan": {
      "label": "サーキュレーター",
      "actions": {
        "toggle": {
          "format": "raw",
          "freq_khz": 38,
          "raw_data": [9000, 4500, 560, 560, 560, 1690]
        }
      }
    }
  }
}
```

### 7.2 書き込み安全性と設定管理

- **アトミック保存:** DB更新時は `/ir_db.tmp` へ書き出してフラッシュした後、`/ir_db.json` へリネームします。書き込み中の電源断によるファイル破損を防ぎます。
- **接続設定の保持:** Wi-Fi情報、APIキー、任意の `RELAY_WS_URL`・`DEVICE_TOKEN` はESP32のNVS（Preferences）またはビルド時設定ファイルで管理します。MVP v0.1はビルド時設定（`secrets.h`）から始め、v0.2でAPモード（Captive Portal）によるスマホ初期設定を追加します。

## 8. M5Stack本体 UI・状態遷移仕様

| 本体状態 | LCD表示内容 | API動作 |
|---|---|---|
| **BOOT / CONNECTING** | Wi-Fi接続中表示 | 受付不可 |
| **IDLE（待機中）** | ローカルIP、`RELAY: ON / OFF`、登録家電数、直近ログ | 全エンドポイント受付可 |
| **LEARNING（学習中）** | 「リモコンを押してください」＋残り秒数カウントダウン | 他の `learn` / `send` は `409 Conflict` を返す |
| **SENDING（送信中）** | 実行中の `device / action` 名 | 送信完了まで同期実行 |
| **ERROR** | エラー概要（Wi-Fi／リレー切断時は自動再接続リトライ） | 復帰後に自動再開 |

## 9. LLMエージェント連携仕様（スキル設計）

エージェント側には `SKILL.md` を同梱し、環境変数 `M5IR_BASE_URL`（ローカルIPまたはCloudflare Workers URL）と `M5IR_API_KEY` を用いてAPIを呼び出します。

### 9.1 家電登録の標準対話ワークフロー

1. エージェントが `GET /api/devices` で既存登録を確認する。
2. ユーザーに「準備ができたら教えてください。M5Stackに向けてリモコンを押してもらいます」と案内し、`POST /api/ir/learn` を実行する。
3. 取得した `capture_id` を使い、`POST /api/ir/test` でテスト送信してユーザーに実機が反応したか確認する。
4. 反応が確認できたら `POST /api/devices/{device_id}/actions` で永続保存する。

### 9.2 安全ルール（スキルへ明記する制約）

- `DELETE` 系エンドポイントを実行する前に、必ず対象名を示してユーザーへ確認を取る。
- 登録名にない家電・操作を求められた場合、勝手に推測して送信せず、既存一覧を提示するか新規学習を提案する。
- マクロ（「おやすみ」等）はエージェント側のスキル／メモリに定義し、実行時は `POST /api/ir/send` の `steps` 配列として1回で送る。

## 10. OSSリポジトリ構成案とロードマップ

```
m5stack-llm-ir/
├── firmware/                 # PlatformIO プロジェクト（M5Stack用C++コード）
│   ├── platformio.ini        # 機種別ビルド環境（cores3 / core2 / stickc 等）
│   ├── include/secrets.h.example
│   └── src/
│       ├── main.cpp          # Wi-Fi・LCD・ローカルHTTPルーティング
│       ├── api_handler.cpp   # HTTP / WebSocket共通のJSONコマンド処理層
│       ├── relay_ws.cpp      # Cloudflare Workers向けWSS逆トンネルクライアント（任意）
│       ├── ir_engine.cpp     # IRremoteESP8266 送受信・学習制御
│       └── storage.cpp       # LittleFS JSON読み書き（アトミック保存）
├── relay/                    # Cloudflare Workers ＋ Durable Objects 中継リレー（任意）
│   ├── wrangler.toml         # SQLite Durable Object バインディング設定
│   └── src/index.ts          # HTTPS ⇔ WebSocket 逆トンネル実装
├── skill/
│   └── SKILL.md              # Hermes / Muse / 汎用CLIエージェント用スキル定義
├── docs/
│   ├── api.md                # APIリファレンス・curlサンプル
│   └── remote-access.md      # Workers逆トンネル / Tailscale / Cloudflare Tunnel ガイド
└── README.md
```

### 将来拡張ロードマップ（Phase 2以降）

- **v0.2:** スマホからWi-Fi・APIキー・リレーURLを設定できるCaptive Portal初期セットアップUI。
- **v0.3:** エアコン向けパラメータ制御（温度・運転モード指定）のオプション対応。
- **v0.4:** [Home Assistant REST API](https://contributors.home-assistant.io/core/internals/rest-api/)互換エンドポイント（`/api/states`, `/api/services`）の追加、リモコン以外のM5 Unit（温湿度・人感・リレー）対応、およびHome Assistant用カスタム統合の同梱。
