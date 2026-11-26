# 宅外アクセスガイド

仕様書 4.2 / 6 章に対応します。どの方式でもエージェント側の API パスと JSON は同一です。
ルーターのポート開放は行いません。

| 方式 | 自宅に常時稼働 PC | ユースケース |
| --- | --- | --- |
| Cloudflare Workers + Durable Objects（WebSocket 逆トンネル） | 不要 | M5Stack 単体で宅外から HTTPS 操作したい |
| Tailscale（Subnet Router） | 必要（ラズパイ等） | 閉域 VPN で直接到達したい |
| Cloudflare Tunnel | 必要（`cloudflared`） | 既存の自宅サーバー構成に載せたい |

---

## 1. Cloudflare Workers + Durable Objects（逆トンネル）

M5Stack は外向き WSS 接続を張るだけなので、自宅側のポート開放も固定 IP も不要です。
Worker が HTTPS 終端と `API_KEY` 認証を行い、Durable Object が M5Stack へリクエストを中継します。

### 1.1 デプロイ

```bash
cd relay
npm install
npx wrangler login

# 2 種類のシークレットを登録（spec 6.4: トークン分離）
npx wrangler secret put API_KEY        # エージェントが使う API キー（32 文字以上推奨）
npx wrangler secret put DEVICE_TOKEN   # M5Stack が WebSocket 接続に使うトークン

npx wrangler deploy
# 例: https://m5ir-relay.<account>.workers.dev
```

`wrangler.toml` の Durable Objects は SQLite バックエンド（`new_sqlite_classes`）で
Free プラン枠内で動作します。

### 1.2 M5Stack 側の設定

リポジトリ直下の `.env`（[`.env.example`](../.env.example) をコピーして作成）:

```dotenv
# エージェント用のAPIキー（Worker の API_KEY と同じ値）
M5IR_API_KEY=エージェント用のAPIキー
# M5Stack の逆トンネル接続先とトークン（Worker の DEVICE_TOKEN と同じ値）
RELAY_WS_URL=wss://m5ir-relay.<account>.workers.dev/ws/device
DEVICE_TOKEN=M5Stack専用のトークン
```

書き込み後、LCD に `Relay: ON`（接続済み）が表示され、`GET /api/health` の
`relay.enabled` / `relay.connected` が `true` になります。

### 1.3 接続の流れ

1. M5Stack が `Authorization: Bearer <DEVICE_TOKEN>` 付きで `wss://.../ws/device` へ接続。
2. Durable Object は単一のアクティブソケットを保持（新規接続時は古いソケットを切断）。
3. エージェントが `https://<worker>/api/...` へ `Authorization: Bearer <API_KEY>` でリクエスト。
4. Worker が認証後、Durable Object が `req_id` を採番し WebSocket フレームで M5Stack へ送信。
5. M5Stack が同じ `api_handler` で処理し、`http_response` フレームで返却。
6. Durable Object が元の HTTP リクエストへステータス・JSON をそのまま返却。

### 1.4 WebSocket フレーム仕様

Relay → M5Stack:

```json
{"type": "http_request", "req_id": "req_01J9X", "method": "POST",
 "path": "/api/ir/send", "body": {"device": "living_light", "action": "off"}}
```

M5Stack → Relay:

```json
{"type": "http_response", "req_id": "req_01J9X", "status": 200,
 "body": {"ok": true, "executed_steps": 1}}
```

Keep-Alive:

* M5Stack は 30 秒間隔で `{"type":"ping"}` を送信し、Relay は `{"type":"pong"}` を返す。
* 応答が途絶えた場合・ソケット切断時は指数バックオフ（2 → 4 → 8 → … 最大 30 秒）で再接続。
* 実装: `firmware/src/relay_ws.cpp`

### 1.5 タイムアウト・オフライン時のエラー

| 異常ケース | HTTP | エラーコード |
| --- | --- | --- |
| M5Stack が WebSocket 未接続 | 503 | `DEVICE_OFFLINE` |
| 通常 API で応答なし | 504 | `DEVICE_TIMEOUT`（10 秒） |
| `/api/ir/learn` で応答なし | 504 | `LEARN_TIMEOUT`（`timeout_ms + 5 秒`、最大 35 秒） |
| `API_KEY` / `DEVICE_TOKEN` 不一致 | 401 | `UNAUTHORIZED`（Worker 入口で拒否） |

### 1.6 ローカルでの動作確認

```bash
cd relay
npx wrangler dev --local --port 8787
# 別ターミナルで
curl -s http://127.0.0.1:8787/health          # Worker 死活監視（認証不要）
```

詳細（テスト手順・`.dev.vars` など）は [`../relay/README.md`](../relay/README.md) を参照してください。

---

## 2. Tailscale（Subnet Router）

自宅 LAN にラズパイ等の常時稼働マシンを置き、サブネットルーターとして公開します。
API をインターネットへ一切公開せず、閉域 VPN 経由で M5Stack のローカル IP へ直接到達できます。

### 2.1 自宅側（Subnet Router）

```bash
# ラズパイ等で
curl -fsSL https://tailscale.com/install.sh | sh
sudo tailscale up --advertise-routes=192.168.1.0/24
```

* Tailscale 管理コンソールで該当マシンの `Subnet routes` を承認（Approve）。
* `192.168.1.0/24` は自宅 LAN の CIDR に置き換えてください。
* M5Stack の IP はルーターで DHCP 固定をおすすめします（LCD / `/api/health` で確認可能）。

### 2.2 エージェント側

同じ tailnet に参加しているマシンから、M5Stack のローカル IP へ直接アクセスします。

```bash
curl -s -H "Authorization: Bearer $M5IR_API_KEY" \
  "http://192.168.1.42/api/health"
```

必要に応じて `M5IR_BASE_URL=http://192.168.1.42` をエージェントに設定します。

---

## 3. Cloudflare Tunnel

既に自宅サーバー（`cloudflared` を常時稼働できるマシン）がある場合の方式です。

```bash
# 自宅サーバーで
cloudflared tunnel login
cloudflared tunnel create m5ir
cloudflared tunnel route dns m5ir m5ir.example.com
cloudflared tunnel run --url http://192.168.1.42:80 m5ir
```

または `~/.cloudflared/config.yml`:

```yaml
tunnel: <TUNNEL_ID>
credentials-file: /home/user/.cloudflared/<TUNNEL_ID>.json
ingress:
  - hostname: m5ir.example.com
    service: http://192.168.1.42:80
  - service: http_status:404
```

* Cloudflare Access の Service Token / ポリシーを併用すると二重防御になります
  （Access を使う場合はエージェント側に `CF-Access-Client-Id` /
  `CF-Access-Client-Secret` ヘッダーを付与）。
* M5Stack 側の Bearer 認証はそのまま有効です。

```bash
curl -s -H "Authorization: Bearer $M5IR_API_KEY" \
  "https://m5ir.example.com/api/health"
```

---

## セキュリティ上の注意

* API キーは 32 文字以上のランダム文字列を使い、`.env` をコミットしない（`.gitignore` 済み）。
* `API_KEY`（エージェント用）と `DEVICE_TOKEN`（M5Stack 接続用）は分離する（spec 6.4）。
* ローカル HTTP では `GET /api/health` 以外が Bearer 認証必須。リレー経由では Worker が
  `/api/*` 全体（health を含む）を `API_KEY` で認証する（Worker 自身の `/health` は認証不要の死活監視）。
* DELETE 系はエージェント側スキルのルールでユーザー確認を取る（`skill/SKILL.md`）。
