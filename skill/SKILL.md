---
name: m5ir-remote
description: M5Stack 製スマートリモコン（m5stack-llm-ir）を HTTP API 経由で操作する。赤外線家電の学習・登録・送信・削除、テスト送信、マクロ（連続バッチ送信）を自然言語の会話から実行する。ユーザーが「テレビをつけて」「照明を消して」「リモコンを登録して」など家電操作を依頼したときに使う。
---

# M5Stack スマートリモコン スキル（m5ir-remote）

M5Stack + Unit IR (U002) のスマートリモコンを操作するスキルです。
エージェントは HTTP API を呼び出すだけで、赤外線の送受信・永続化はデバイス側が行います。

## 環境変数

| 変数 | 必須 | 説明 |
| --- | --- | --- |
| `M5IR_BASE_URL` | 必須 | `http://m5ir.local`、`http://<固定IP>`、または Cloudflare Workers の URL（`https://xxx.workers.dev`） |
| `M5IR_API_KEY` | 必須 | `Authorization: Bearer <API_KEY>` に使う API キー（32 文字以上） |

```bash
export M5IR_BASE_URL="http://m5ir.local"
export M5IR_API_KEY="0123456789abcdef0123456789abcdef"
```

## 最初に必ず行うこと

1. `GET /api/health` で疎通と状態を確認する（ローカル直結は認証不要、Workers リレー経由は
   `Authorization: Bearer $M5IR_API_KEY` が必要）。
2. `ir.hardware` を確認する:
   * `true` → 赤外線ユニット接続済み。学習・送信が可能。
   * `false` → **Unit IR 未接続**（またはスタブビルド）。IR 系 API は 503
     `IR_NOT_AVAILABLE` を返す。ユーザーに「赤外線ユニットが未接続なので、届いて
     接続・`-ir` ビルドするまで学習/送信はできません」と正直に伝える。推測で送信しない。
3. `GET /api/devices` で登録済み家電を確認する。

```bash
curl -s -H "Authorization: Bearer $M5IR_API_KEY" "$M5IR_BASE_URL/api/health" | python3 -m json.tool
curl -s -H "Authorization: Bearer $M5IR_API_KEY" "$M5IR_BASE_URL/api/devices" | python3 -m json.tool
```

## 家電・操作の登録（標準ワークフロー）

仕様 9.1 の対話フローです。

1. **既存登録の確認**
   ```bash
   curl -s -H "Authorization: Bearer $M5IR_API_KEY" "$M5IR_BASE_URL/api/devices"
   ```
2. **ユーザーへの案内**
   「準備ができたら教えてください。M5Stack に向けてリモコンを押してもらいます」と伝える。
3. **学習（受光待機）**
   ```bash
   curl -s -X POST "$M5IR_BASE_URL/api/ir/learn" \
     -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
     -d '{"timeout_ms": 15000}'
   ```
   * 既定 10 秒、最大 30 秒。ユーザーが押すまで待つので、HTTP クライアントの
     タイムアウトは `timeout_ms + 5 秒` 以上に設定する。
   * 応答例: `{"ok":true,"capture_id":"cap_8f2a","protocol":"NEC","bits":32,"raw_length":67}`
   * `capture_id` は 10 分で失効する。すぐに次のステップへ進む。
4. **テスト送信して実機の反応を確認**
   ```bash
   curl -s -X POST "$M5IR_BASE_URL/api/ir/test" \
     -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
     -d '{"capture_id":"cap_8f2a"}'
   ```
   * 「テレビが反応しましたか？」とユーザーに確認する。反応しない場合は
     やり直し（再学習）を提案する。勝手に別の家電として登録しない。
5. **永続保存**
   ```bash
   curl -s -X POST "$M5IR_BASE_URL/api/devices/living_tv/actions" \
     -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
     -d '{"label":"リビングのテレビ","action":"power","capture_id":"cap_8f2a"}'
   ```
   * `device_id`: 英小文字・数字・アンダースコアのみ（1〜24 文字）。
   * `action`: 英小文字・数字・アンダースコアのみ（1〜32 文字）。
   * 応答例: `{"ok":true,"device_id":"living_tv","action":"power","stored_format":"protocol"}`
6. 登録できたら `GET /api/devices` で一覧を再確認し、ユーザーに報告する。

## 家電操作（送信）

### 単発

```bash
curl -s -X POST "$M5IR_BASE_URL/api/ir/send" \
  -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
  -d '{"device":"living_light","action":"off"}'
```

### マクロ（連続バッチ）

```bash
curl -s -X POST "$M5IR_BASE_URL/api/ir/send" \
  -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
  -d '{"steps":[
        {"device":"living_tv","action":"power"},
        {"delay_ms":400},
        {"device":"living_light","action":"off"}
      ]}'
```

* `steps` は最大 20、`delay_ms` は 1 ステップあたり 0〜5000 ms。
* マクロ（「おやすみ」「映画モード」など）は**このスキルのメモリ／会話内に定義**し、
  実行時に `steps` へ展開して 1 回で送る（本体には保存しない。仕様 1.2）。
  * 例: 「おやすみ」= テレビ power off → 400ms → 照明 off
* 送信結果は `executed_steps` で確認する。

## 削除

`DELETE` を実行する前に、**必ず対象名（家電名・アクション名）をユーザーに示して確認を取る**。

```bash
# 家電ごと削除（配下の全アクションも消える）
curl -s -X DELETE "$M5IR_BASE_URL/api/devices/living_tv" \
  -H "Authorization: Bearer $M5IR_API_KEY"

# アクション単位で削除
curl -s -X DELETE "$M5IR_BASE_URL/api/devices/living_tv/actions/mute" \
  -H "Authorization: Bearer $M5IR_API_KEY"
```

## 安全ルール（必ず守る）

1. **削除前の確認**: `DELETE` 系の前に、対象名を示してユーザーの同意を得る。
2. **推測禁止**: 登録にない家電・操作を頼まれたら、勝手に推測して送信しない。
   既存一覧（`GET /api/devices`）を提示するか、新規学習を提案する。
3. **未接続時は正直に**: `ir.hardware=false` / `503 IR_NOT_AVAILABLE` のときは、
   IR ユニット未接続であることを伝える。成功したフリをしない。
4. **二重登録の確認**: 同じ家電・アクションが既にある場合は、上書きしてよいか確認する。
5. **タイムアウト**: 学習リクエストは最大 30 秒ブロックする。クライアント側タイムアウトを
   `timeout_ms + 5 秒` 以上に設定する。リレー経由の場合は最大 35 秒。

## エラーハンドリング

| 応答 | 対応 |
| --- | --- |
| `401 UNAUTHORIZED` | `M5IR_API_KEY` を確認。ユーザーに設定漏れを伝える。 |
| `404 DEVICE_NOT_FOUND` / `ACTION_NOT_FOUND` | 一覧を提示し、登録するか確認する。 |
| `404 CAPTURE_NOT_FOUND` | `capture_id` の失効。再学習する。 |
| `408 LEARN_TIMEOUT` | リモコンが押されなかった。もう一度試すか確認する。 |
| `409 IR_BUSY` | 他の学習/送信中。数秒待って 1 回だけ再試行する。 |
| `503 IR_NOT_AVAILABLE` | Unit IR 未接続。ユーザーに説明する。 |
| `503 DEVICE_OFFLINE` | （リレー）M5Stack が未接続。本体の電源・Wi-Fi・relay 設定を確認。 |
| `504 DEVICE_TIMEOUT` / `LEARN_TIMEOUT` | （リレー）応答なし。後で再試行し、改善しなければ M5Stack 側を確認。 |
| `500 STORAGE_ERROR` | LittleFS 保存失敗。再試行し、直らなければ本体ログを確認。 |

## 補足

* 状態は `GET /api/health` の `state` で `idle` / `learning` / `sending` / `error` を確認できる。
* 本体 LCD には IP、Relay 状態、学習カウントダウンが表示される。
* 詳細な API 仕様はリポジトリの `docs/api.md` を参照。
