# relay — Cloudflare Workers 逆トンネル中継

M5Stack IR リモートの **ローカル HTTP API を、ルーターのポート開放なしで `https://` 経由で操作できるようにする** 任意コンポーネントです。
Cloudflare Workers + SQLite バックエンドの Durable Objects（WebSocket Hibernation API）だけで動作し、Workers Free プランの範囲で無料運用できます。

```
LLM エージェント ──HTTPS + API_KEY──> Worker ──> Relay Durable Object
                                                       │ WebSocket（M5Stack から外向き接続）
                                                       v
                                                  M5Stack ファームウェア
```

- Worker が `Authorization: Bearer <API_KEY>` を検証してから Durable Object に転送します（未認証リクエストは DO に一切到達しません）。
- M5Stack は `GET /ws/device` に `Authorization: Bearer <DEVICE_TOKEN>` で WebSocket 接続します。
- Durable Object は常に **1 台分の有効なデバイスソケットだけ** を保持し、再接続時は古いソケットを `1000 / "replaced"` で閉じます（再起動後のゴースト接続対策）。
- エージェント向け API パス・JSON 仕様はローカル API（`spec/SPEC.md` 5章）と完全に同一です。

## 必要環境

- Node.js 20 以上（このリポジトリの検証は Node.js 26 で実施）
- Cloudflare アカウント（無料枠で可）
- npm

## セットアップとデプロイ

```bash
cd relay
npm install

# Cloudflare にログイン（初回のみ。ブラウザが開きます）
npx wrangler login

# シークレット登録（2つは別々の値にしてください。API_KEY は32文字以上のランダム推奨）
npx wrangler secret put API_KEY
npx wrangler secret put DEVICE_TOKEN

# デプロイ
npx wrangler deploy
```

デプロイすると次のような URL が表示されます。

```
https://m5stack-llm-ir-relay.<your-subdomain>.workers.dev
```

| 用途 | URL / 設定値 |
|---|---|
| エージェントのベース URL（`M5IR_BASE_URL`） | `https://m5stack-llm-ir-relay.<your-subdomain>.workers.dev` |
| エージェントの API キー（`M5IR_API_KEY`） | `wrangler secret put API_KEY` で登録した値 |
| ファームウェアの `RELAY_WS_URL` | `wss://m5stack-llm-ir-relay.<your-subdomain>.workers.dev/ws/device` |
| ファームウェアの `DEVICE_TOKEN` | `wrangler secret put DEVICE_TOKEN` で登録した値 |

> `API_KEY` と `DEVICE_TOKEN` は Worker のソースにも `wrangler.toml` にも書かないでください。`wrangler secret put` で登録し、ローカル開発時のみ `.dev.vars`（gitignore 済み）を使います。

## 公開エンドポイント

| メソッド | パス | 認証 | 内容 |
|---|---|---|---|
| `GET` | `/health` | なし | Worker の稼働とデバイス接続状態を返す（`{"ok":true,"worker":true,"device_online":true}`） |
| `GET` | `/ws/device` | `Bearer <DEVICE_TOKEN>` | M5Stack 用 WebSocket アップグレード |
| 任意 | `/api/*` | `Bearer <API_KEY>` | M5Stack のローカル API へ中継（`/api/health` を含む） |

- `/api/*` 以外の未知パスは Worker が `404 NOT_FOUND` を返します。
- `/api/*` の未知パスは M5Stack にそのまま転送され、M5Stack の `404` が返ります。
- **注意:** ローカル API では認証任意の `GET /api/health` も、リレー経由では `API_KEY` 必須です。認証なしの死活確認には `GET /health` を使ってください。

## ローカル開発

`relay/.dev.vars` を作成します（コミット禁止。`.gitignore` 済み）。

```dotenv
API_KEY=test-api-key
DEVICE_TOKEN=test-device-token
```

```bash
npm run dev
# または: npx wrangler dev --local --port 8787
```

動作確認（別ターミナル。デバイス未接続なので `503 DEVICE_OFFLINE` が正常です）:

```bash
# 認証不要のヘルスチェック
curl -s http://127.0.0.1:8787/health
# => {"ok":true,"worker":true,"device_online":false}

# API_KEY なし / 誤り -> 401
curl -si http://127.0.0.1:8787/api/devices | head -1
# => HTTP/1.1 401 Unauthorized

# 正しい API_KEY・デバイス未接続 -> 503 DEVICE_OFFLINE
curl -s -H 'Authorization: Bearer test-api-key' http://127.0.0.1:8787/api/devices
# => {"ok":false,"error":{"code":"DEVICE_OFFLINE","message":"..."}}

# ボディ不正 JSON -> 400 INVALID_JSON
curl -s -X POST -H 'Authorization: Bearer test-api-key' \
  -H 'Content-Type: application/json' -d '{oops' \
  http://127.0.0.1:8787/api/ir/send
```

### ローカル統合テスト

`npm test` は `wrangler dev` を自動起動し、ダミーの M5Stack WebSocket クライアントを接続して往復・タイムアウト・再接続（置換）を検証します。`npm test` 実行時は `.dev.vars` が無ければテスト用の値を `--var` で渡します。既に起動済みのサーバーに対して実行する場合は `RELAY_URL=http://127.0.0.1:8787 npm test` としてください。

```bash
npm test
```

## エージェントからの利用例

```bash
BASE=https://m5stack-llm-ir-relay.<your-subdomain>.workers.dev
KEY=<API_KEY>

curl -s -H "Authorization: Bearer $KEY" "$BASE/api/devices"

curl -s -X POST -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
  -d '{"device":"living_light","action":"off"}' "$BASE/api/ir/send"
```

リクエストのメソッド・JSON ボディ・パスはそのまま M5Stack に渡り、M5Stack が返した HTTP ステータスと JSON ボディがそのまま返ります。クエリ文字列は MVP の API には存在しないため転送しません（パスのみ）。

## エラー一覧（`spec/SPEC.md` 6.3）

| 状況 | HTTP | `error.code` |
|---|---|---|
| `API_KEY` / `DEVICE_TOKEN` 不一致 | `401` | `UNAUTHORIZED` |
| リクエストボディが不正な JSON | `400` | `INVALID_JSON` |
| M5Stack が未接続 | `503` | `DEVICE_OFFLINE` |
| 通常 API の応答なし（10秒） | `504` | `DEVICE_TIMEOUT` |
| `/api/ir/learn` の応答なし（`timeout_ms + 5秒`、最大35秒） | `504` | `LEARN_TIMEOUT` |
| `/api/*` 以外の未知パス | `404` | `NOT_FOUND` |
| `/health` への `GET` 以外（`/ws/device` 以外のメソッド等） | `405` | `METHOD_NOT_ALLOWED` |
| 中継内部エラー（DO 到達不能・デバイス応答不正など） | `502` | `BAD_GATEWAY` |

タイムアウト時間の詳細:

- 通常リクエスト: 10 秒。
- `POST /api/ir/learn`: リクエストの `timeout_ms` を `1〜30000` にクランプ（欠落・不正時は `10000`）し、`timeout_ms + 5000ms`（最大 35000ms）で待機します。
- デバイスが切断・再接続した場合、そのデバイスに送信済みで未応答のリクエストは即座に `503 DEVICE_OFFLINE` で失敗します。

## ファームウェア実装者向けメモ（フレーム仕様）

Relay → M5Stack（リクエスト転送）:

```json
{"type":"http_request","req_id":"req_<uuid>","method":"POST","path":"/api/ir/send","body":{"device":"living_light","action":"off"}}
```

- `req_id`: Relay が採番する一意な文字列。**応答フレームには必ず同じ `req_id` をそのまま返してください。**
- `method`: `GET` / `POST` / `DELETE` など大文字。`path`: 先頭 `/` を含むパス（クエリなし）。
- `body`: パース済み JSON オブジェクト、またはボディ無し/空の場合は `null`。

M5Stack → Relay（応答返却）:

```json
{"type":"http_response","req_id":"req_<uuid>","status":200,"body":{"ok":true,"executed_steps":1}}
```

- `status`: 数値（200〜599）。`body`: ローカル HTTP API が返すのと同じ JSON。ステータスコードと JSON ボディがエージェントへそのまま返ります。
- 同じ `req_id` への応答は最初の 1 回だけ有効です（タイムアウト後に届いた応答は破棄されます）。

Keep-Alive:

- M5Stack から `{"type":"ping"}` を送ると Relay が `{"type":"pong"}` を返します。30秒間隔を推奨。
- WebSocket 制御フレームの Ping には Workers ランタイムが自動で Pong を返します（アプリ側での処理は不要）。
- 切断を検知したら指数バックオフ（2→4→8→…最大30秒）で `/ws/device` へ再接続してください。再接続すると Relay 側は新ソケットを有効化し、古いソケットを `1000 "replaced"` で閉じます。
- 接続時のヘッダーは `Authorization: Bearer <DEVICE_TOKEN>` のみ必須です（`Upgrade: websocket` は WebSocket クライアントが自動付与）。

## 構成

```
relay/
├── package.json
├── tsconfig.json          # strict / ES2022 / @cloudflare/workers-types
├── wrangler.toml          # RELAY_DO バインディング + SQLite クラス移行
├── .dev.vars              # ローカル専用（gitignore）
├── src/
│   ├── index.ts           # Worker ルーティング + Relay Durable Object
│   └── types.ts           # フレーム型・Env 型
└── test/
    └── integration.mjs    # ローカル統合テスト（ws クライアント）
```

## トラブルシューティング

### M5Stack が `device_online: false` のまま

1. ファームウェアの `.env` に `RELAY_WS_URL`（`wss://<worker>/ws/device`）と
   `DEVICE_TOKEN` が入っているか確認（Worker の `DEVICE_TOKEN` と一致させる）。
2. シリアルログ（115200 baud）で `[relay] connected` が出るか確認。
3. デバイスは WSS 接続前に NTP で時刻同期します。NTP が通らないネットワークでは
   TLS が失敗することがあります。
4. 管理画面の `wrangler tail` で Worker / Durable Object のログを確認。

### WebSocket が 1002 (protocol error) で即切断される

ESP32 側の `setExtraHeaders()` に渡す文字列の末尾へ `\r\n` を付けると、
HTTP ヘッダーブロックが早期終了し、後続のヘッダー行が WebSocket フレームとして
送信されてしまいます（Cloudflare は 1002 で切断）。末尾 CRLF は付けないでください。
本リポジトリの `firmware/src/relay_ws.cpp` は修正済みです。

### WebSocket アップグレードが 500 になる

Worker が 101 レスポンスを再構築すると `webSocket` ハンドルが失われ 500 になります。
本リポジトリの Worker は `/ws/device` だけ CORS ラッパーを通さず返却しています。
