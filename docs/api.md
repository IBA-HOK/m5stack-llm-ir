# HTTP API リファレンス（v1）

仕様書 5 章の実装に基づく API リファレンスです。ローカル HTTP でも
Cloudflare Workers リレー経由でも、パスと JSON は完全に同一です。

## ベース URL

| 経路 | ベース URL の例 |
| --- | --- |
| ローカル LAN | `http://m5ir.local` または `http://<固定IP>` |
| Workers 逆トンネル | `https://<worker-subdomain>.workers.dev` |
| Tailscale / Cloudflare Tunnel | 各トンネルのホスト名 |

```bash
export M5IR_BASE_URL="http://m5ir.local"
export M5IR_API_KEY="0123456789abcdef0123456789abcdef"   # .env の M5IR_API_KEY と同じ値
```

## 共通仕様

* `Content-Type: application/json; charset=utf-8`
* 成功レスポンス: `{"ok": true, ...}`
* エラーレスポンス:

  ```json
  {"ok": false, "error": {"code": "ERROR_CODE", "message": "詳細"}}
  ```

* `Authorization: Bearer <API_KEY>` ヘッダー必須（ローカル HTTP では `GET /api/health` のみ任意。
  Workers リレー経由では Worker が `/api/*` 全体を認証するため health にも必要）
* 主なステータスコード: `200` / `201` / `400` / `401` / `404` / `405` /
  `408`（学習タイムアウト）/ `409`（学習・送信中などのビジー）/ `413` /
  `500` / `503` / `504`（リレー経由）

### アプリケーションエラーコード

| コード | HTTP | 意味 |
| --- | --- | --- |
| `INVALID_REQUEST` | 400 | パラメータ不正 |
| `INVALID_JSON` | 400 | ボディが JSON として不正 |
| `INVALID_DEVICE_ID` | 400 | device_id が `[a-z0-9_]` 以外、または長すぎる |
| `INVALID_ACTION` | 400 | action 名が `[a-z0-9_]` 以外、または長すぎる |
| `STEPS_LIMIT_EXCEEDED` | 400 | `steps` が 20 件超 |
| `UNAUTHORIZED` | 401 | Bearer トークン不一致 |
| `CAPTURE_NOT_FOUND` | 404 | `capture_id` が存在しない/期限切れ |
| `DEVICE_NOT_FOUND` | 404 | 家電が登録されていない |
| `ACTION_NOT_FOUND` | 404 | アクションが登録されていない |
| `NOT_FOUND` | 404 | 未知のパス |
| `METHOD_NOT_ALLOWED` | 405 | メソッド不一致 |
| `LEARN_TIMEOUT` | 408 | 赤外線信号を受信できずタイムアウト |
| `IR_BUSY` | 409 | 学習中/送信中で実行できない |
| `PAYLOAD_TOO_LARGE` | 413 | ボディが 8 KB 超 |
| `IR_SEND_FAILED` / `IR_UNSUPPORTED_PROTOCOL` / `IR_INVALID_SIGNAL` | 500 | 送信失敗（信号不正含む） |
| `STORAGE_ERROR` | 500 | LittleFS 保存失敗 |
| `IR_NOT_AVAILABLE` | 503 | IR ユニット未接続（スタブビルド） |
| `CAPTURE_BUFFER_FULL` | 503 | キャプチャ保持に失敗 |
| `CONFIG_MISSING` | 503 | `M5IR_API_KEY` 未設定 |
| `DEVICE_OFFLINE` | 503 | （リレー）M5Stack が WebSocket 未接続 |
| `DEVICE_TIMEOUT` | 504 | （リレー）M5Stack 応答なし（10 秒） |
| `LEARN_TIMEOUT` | 504 | （リレー）学習応答なし（`timeout_ms + 5 s`、最大 35 秒） |

---

## GET /api/health

稼働確認・Wi-Fi・空きフラッシュ・現在状態を返します。認証はローカル HTTP では任意、
Workers リレー経由では `Authorization: Bearer <API_KEY>` が必要です。

```bash
# ローカル（トークンなしでも可）
curl -s "$M5IR_BASE_URL/api/health" | python3 -m json.tool

# リレー経由（Worker が /api/* を認証）
curl -s -H "Authorization: Bearer $M5IR_API_KEY" "$M5IR_BASE_URL/api/health" | python3 -m json.tool
```

```json
{
  "ok": true,
  "version": "0.1.0",
  "state": "idle",
  "uptime_ms": 123456,
  "auth_required": true,
  "wifi": {
    "connected": true,
    "ssid": "my-home",
    "ip": "192.168.1.42",
    "rssi": -58,
    "hostname": "m5ir.local"
  },
  "filesystem": {"total_bytes": 1507328, "used_bytes": 4096, "free_bytes": 1503232},
  "ir": {
    "hardware": false,
    "backend": "stub",
    "tx_pin": 9,
    "rx_pin": 8,
    "reason": "IR unit is not connected (firmware built without M5IR_ENABLE_IR_HW=1)"
  },
  "relay": {"enabled": false, "connected": false},
  "devices": 2,
  "message": "ready",
  "learn_remaining_ms": 0,
  "learn_target": "",
  "reset_reason": "power_on",
  "free_heap": 169116
}
```

* `state`: `boot` / `connecting` / `idle` / `learning` / `sending` / `error`
* `reset_reason`: 起動理由（`power_on` / `software` / `panic` / `task_watchdog` / `brownout` など）。再起動トラブルの切り分け用。
* `free_heap`: 空きヒープ量（バイト）。
* `ir.hardware=false` は「Unit IR 未接続（スタブビルド）」を意味します。
* `ir.backend`: `stub`（Unit IR 未接続）/ `fake`（試験用の合成バックエンド）/ `irremote`（実機 IR）。
  届いたら `-ir` 環境で書き込み直してください（[hardware.md](hardware.md)）。

## GET /api/devices

登録済みの家電・アクション一覧。

```bash
curl -s -H "Authorization: Bearer $M5IR_API_KEY" "$M5IR_BASE_URL/api/devices" \
  | python3 -m json.tool
```

```json
{
  "ok": true,
  "devices": {
    "living_tv": {"label": "リビングのテレビ", "actions": ["mute", "power", "vol_down", "vol_up"]},
    "living_light": {"label": "リビング照明", "actions": ["night", "off", "on"]}
  }
}
```

## POST /api/ir/learn

指定時間だけ赤外線を受光待機し、取得した信号を一時保存して `capture_id` を返します。
生波形はレスポンスに含まれません（仕様 5.3）。

```bash
curl -s -X POST "$M5IR_BASE_URL/api/ir/learn" \
  -H "Authorization: Bearer $M5IR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"timeout_ms": 10000}' | python3 -m json.tool
```

```json
{"ok": true, "capture_id": "cap_8f2a", "protocol": "NEC", "bits": 32, "raw_length": 67}
```

| パラメータ | 型 | 既定 | 制約 |
| --- | --- | --- | --- |
| `timeout_ms` | int | 10000 | 500〜30000（上限は仕様 4.3） |
| `device` | string | 任意 | 表示用。M5 画面に `LEARN: <device>/<action>` と出す |
| `action` | string | 任意 | 表示用。上記と組み合わせる（1〜32 文字、`[a-z0-9_]`） |

* 学習中の別の `learn` / `send` は **409 `IR_BUSY`**。
* タイムアウトは **408 `LEARN_TIMEOUT`**。
* キャプチャは RAM 上に最新 3 件・10 分間保持（再起動で消える）。

## POST /api/ir/test

一時保存中の `capture_id` をテスト送信します。

```bash
curl -s -X POST "$M5IR_BASE_URL/api/ir/test" \
  -H "Authorization: Bearer $M5IR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"capture_id": "cap_8f2a"}' | python3 -m json.tool
```

```json
{"ok": true, "sent": true, "capture_id": "cap_8f2a"}
```

## POST /api/devices/{device_id}/actions

`capture_id` を指定した家電・アクション名で LittleFS へ永続保存します（新規作成・上書き）。

```bash
curl -s -X POST "$M5IR_BASE_URL/api/devices/living_tv/actions" \
  -H "Authorization: Bearer $M5IR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"label": "リビングのテレビ", "action": "power", "capture_id": "cap_8f2a"}' \
  | python3 -m json.tool
```

```json
{"ok": true, "device_id": "living_tv", "action": "power", "stored_format": "protocol"}
```

* `device_id` は `[a-z0-9_]`（1〜24 文字）のみ。
* `action` も `[a-z0-9_]`（1〜32 文字）のみ。
* 新規家電の場合は `label` 必須。既存家電なら省略可（省略時はラベル維持）。
* `format: "protocol"`（NEC 等）または `format: "raw"`（未知プロトコル）で保存されます。

## POST /api/ir/send

登録済みの単発アクション、または複数ステップの連続バッチを送信します。

### 単発

```bash
curl -s -X POST "$M5IR_BASE_URL/api/ir/send" \
  -H "Authorization: Bearer $M5IR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"device": "living_light", "action": "off"}' | python3 -m json.tool
```

### バッチ（マクロ）

```bash
curl -s -X POST "$M5IR_BASE_URL/api/ir/send" \
  -H "Authorization: Bearer $M5IR_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"steps": [
        {"device": "living_tv", "action": "power"},
        {"delay_ms": 400},
        {"device": "living_light", "action": "off"}
      ]}' | python3 -m json.tool
```

```json
{"ok": true, "executed_steps": 3}
```

* `steps` は最大 20。`delay_ms` は 1 ステップあたり 0〜5000（仕様 4.3）。
* 送信前に全ステップの家電・アクションを検証します（未登録があれば何も送信せず 404）。
* 送信中は他の `learn` / `send` が **409 `IR_BUSY`** になります。

## DELETE /api/devices/{device_id}

家電と配下の全アクションを削除します。

```bash
curl -s -X DELETE "$M5IR_BASE_URL/api/devices/living_tv" \
  -H "Authorization: Bearer $M5IR_API_KEY" | python3 -m json.tool
```

```json
{"ok": true, "device_id": "living_tv"}
```

## DELETE /api/devices/{device_id}/actions/{action}

特定アクションのみを削除します。

```bash
curl -s -X DELETE "$M5IR_BASE_URL/api/devices/living_tv/actions/mute" \
  -H "Authorization: Bearer $M5IR_API_KEY" | python3 -m json.tool
```

```json
{"ok": true, "device_id": "living_tv", "action": "mute"}
```

---

## 状態遷移と排他制御

| 本体状態 | API 動作 |
| --- | --- |
| `idle` | 全エンドポイント受付 |
| `learning` | 他の `learn` / `send` は `409 IR_BUSY` |
| `sending` | 他の `learn` / `send` は `409 IR_BUSY`（バッチは同期実行） |
| `error` | Wi-Fi 切断時など。復帰後に自動再開 |

HTTP サーバーはリクエストごとに非同期ワーカータスクで処理するため、学習中でも
`GET /api/health` などは応答します（`409` を返せます）。

## マクロ（シーン）

MVP ではマクロを本体に保存せず、エージェント側のスキル/メモリに定義します（仕様 1.2）。
実行時は `POST /api/ir/send` の `steps` 配列として 1 回で送ります。
