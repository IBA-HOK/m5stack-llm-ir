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
2. `ir.hardware` と `ir.backend` を確認する:
   * `backend: "irremote"`（`hardware: true`）→ 実機の Unit IR。学習・送信が可能。
   * `backend: "stub"`（`hardware: false`）→ **Unit IR 未接続**。IR 系 API は 503
     `IR_NOT_AVAILABLE` を返す。ユーザーに「赤外線ユニットが未接続なので、届いて
     接続・`-ir` ビルドするまで学習/送信はできません」と正直に伝える。推測で送信しない。
     * health の時点で `stub` と分かるので、`learn` を呼ばずに説明だけしてもよい。
       実挙動の確認を兼ねて 1 回だけ `learn` を試し、503 を確認するのも可。
       いずれの場合も、登録・送信・架空 `capture_id` の使用はしない。
   * `backend: "fake"`（`hardware: true` に見える）→ **試験用の合成バックエンド**。
     学習・送信 API は成功したように見えるが、実機の赤外線は一切出ていない。
     * API／登録フローの動作確認目的であれば learn / test / send を実行してよいが、
       結果は「API が受理した」として報告する。
     * 「テレビが反応した」など実機動作のように報告してはならない。
     * ユーザーに「リモコンを押してください」と案内する必要はない（合成信号が即返る）。
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
   自動実行などで人間がリモコンを押せない場合は、待機に入る前にその旨を報告し、
   学習を成功させない（`408 LEARN_TIMEOUT` / `503 IR_NOT_AVAILABLE` は正直に扱う）。
3. **学習（受光待機）**
   ```bash
   curl -s -X POST "$M5IR_BASE_URL/api/ir/learn" \
     -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
     -d '{"timeout_ms": 15000}'
   ```
   * 既定 10 秒、最大 30 秒。ユーザーが押すまで待つので、HTTP クライアントの
     タイムアウトは `timeout_ms` **より大きい値**（目安: +5 秒）に設定する。
     リレー経由はサーバー側の上限が 35 秒なので、`timeout_ms` は 30 秒以下にする。
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
   * 複数アクションを登録する場合も、**各アクションごとに学習→テスト→登録**を
     基本とする（ユーザーがテスト省略を明示した場合のみ省略可）。
   * 人間が確認できない自動実行では、`sent:true` は「送信リクエストが受理された」
     ことのみを意味する。実機が反応したかは不明であると報告し、断定しない。
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
* 送信結果は `executed_steps` で確認する。`executed_steps` は `steps` 配列の要素数
  （送信ステップと `delay_ms` ステップの合計）。例: 送信 2 件 + `delay_ms` 1 件 = 3。

## 削除

`DELETE` を実行する前に、**必ず対象名（家電名・アクション名）をユーザーに示して確認を取る**。

* ユーザーが同一リクエスト内で対象を明示して削除を事前了承している場合は、
  対象（`device_id` / label / actions）を復唱したうえで実行してよい。
* 削除後は `GET /api/devices` で対象が消えたことを確認して報告する。

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
3. **未接続・疑似バックエンドでは正直に**: `ir.hardware=false` / `503 IR_NOT_AVAILABLE`
   は Unit IR 未接続として説明する。`ir.backend="fake"` は試験用の合成バックエンドで
   あり、実機の赤外線送信ではないと明記する。成功したフリをしない。
4. **二重登録の確認**: 同じ家電・アクションが既にある場合は、上書きしてよいか確認する。
5. **タイムアウト**: 学習リクエストは最大 30 秒ブロックする。クライアント側タイムアウトを
   `timeout_ms` より大きい値（目安 +5 秒、リレー経由はサーバー上限 35 秒）に設定する。
6. **非対話実行時の報告**: 人間が確認できない自動実行では、HTTP 200 / `sent:true` は
   「API が受理した」意味に留める。家電の実反応は未確認である旨を報告する。

## エラーハンドリング

| 応答 | 対応 |
| --- | --- |
| `401 UNAUTHORIZED` | `M5IR_API_KEY` を確認。ユーザーに設定漏れを伝える。 |
| `404 DEVICE_NOT_FOUND` / `ACTION_NOT_FOUND` | 一覧を提示し、登録するか確認する。 |
| `404 CAPTURE_NOT_FOUND` | `capture_id` の失効。再学習する。 |
| `408 LEARN_TIMEOUT` | リモコンが押されなかった。もう一度試すか確認する。 |
| `409 IR_BUSY` | 他の学習/送信中。数秒待って 1 回だけ再試行する。 |
| `503 IR_NOT_AVAILABLE` | Unit IR 未接続（`ir.backend="stub"`）。ユーザーに説明する。 |
| `503 DEVICE_OFFLINE` | （リレー）M5Stack が未接続。本体の電源・Wi-Fi・relay 設定を確認。 |
| `504 DEVICE_TIMEOUT` / `LEARN_TIMEOUT` | （リレー）応答なし。後で再試行し、改善しなければ M5Stack 側を確認。 |
| `500 STORAGE_ERROR` | LittleFS 保存失敗。再試行し、直らなければ本体ログを確認。 |

## 成功時のステータスと ID 形式

* アクションの永続保存 `POST /api/devices/{device_id}/actions` は **201 Created**。
* `DELETE /api/devices/{device_id}` と `DELETE .../actions/{action}` は **200 OK**
  （JSON ボディあり。204 ではない）。
* それ以外の成功レスポンスは **200 OK**。
* `capture_id` は `cap_` + 4 桁の 16 進数（例: `cap_1090`, `cap_b9f3`）。書式を仮定せず、
  レスポンスの値をそのまま使う。

## 補足

* 状態は `GET /api/health` の `state` で `idle` / `learning` / `sending` / `error` を確認できる。
* 本体 LCD には IP、Relay 状態、学習カウントダウンが表示される。
* 詳細な API 仕様はリポジトリの `docs/api.md` を参照。
