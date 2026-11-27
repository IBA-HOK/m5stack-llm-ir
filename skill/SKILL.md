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

## 家電・操作の登録（標準ワークフロー：ガイド付き）

**このスキルの最重要ポイント**: 学習の待機中に黙って待たない。
「どの家電の、どのボタン」を具体的に案内してから学習を開始する。

### ガイドの原則

1. 抽象的な「リモコンを押してください」ではなく、**「テレビのリモコンの『1』」**のように
   家電とボタンを具体的に言う。
2. 学習の案内文を出した**直後に** `learn` を呼ぶ（案内と受付開始を一致させる）。
3. 1回の学習で1ボタン。複数登録する場合は1つずつ案内する。
4. M5Stack 本体の画面には学習ターゲットが **`LEARN: <device>/<action>`** と
   残り秒数で表示される。ユーザーにもその表示に触れて安心させる。
5. タイムアウト（408）しても責めず、向き・距離・ボタン・電池を具体的に再案内する。

### 手順

1. **依頼の具体化**
   - どの家電（`device_id` / label）の、どのボタン（`action`）かを確定する。
   - あいまいな場合（例:「テレビを登録して」）は「電源ボタンから登録しましょう」など
     最初の1ボタンを提案して合意する。
   - `action` 名は `[a-z0-9_]` の意味ある名前を提案する
     （数字ボタン → `ch1` / `button_1`、電源 → `power`、音量+ → `vol_up`）。
     ユーザー指定があればそれに従い、ガイド文面にも action 名を含める。
2. **既存登録の確認と重複判定**
   ```bash
   curl -s -H "Authorization: Bearer $M5IR_API_KEY" "$M5IR_BASE_URL/api/devices"
   ```
   応答は `device_id` をキーにしたオブジェクト:
   ```json
   {"ok":true,"devices":{"test_tv":{"label":"テストテレビ","actions":["power"]}}}
   ```

   結果と依頼内容を比較し、次のいずれかに分類する。

   | 状況 | 判定 |
   | --- | --- |
   | 同じ `device_id` + 同じ `action` がある | **重複確実**（保存すると上書き） |
   | 同じ/似た label で `device_id` が違う | **重複の可能性**（同じ家電の二重登録） |
   | 同じ `device_id` で新しい `action` | 重複ではない（操作の追加）→ 続行 |

   「似た label」の目安: 完全一致、片方が他方を含む（「テレビ」⊂「リビングのテレビ」）、
   接尾辞違い（`tv` / `tv_2`）は類似とみなす。迷う場合は重複の可能性として扱う。

   **重複の可能性がある間は、GET 以外（`learn` / `test` / `save` / `send` / `delete`）を
   一切実行しない。** 勝手に上書き・削除・新規追加もしない。必ず次の3択をユーザーに聞く
   （対象名を具体的に示す。同じ家電らしき別 device があれば併せて言及する）。

   > 「`test_tv/power` は既に登録済みです。どうしますか？
   > ① **上書き更新**（今の内容を新しい信号で置き換える）
   > ② **削除して作り直す**（既存を消してから登録）
   > ③ **別名で新規追加**（例: `test_tv_2/power` として追加）」

   選択に応じた動き:
   - ① 上書き: 学習→テスト→保存（同じ device_id/action）。成功すれば既存は置き換わる。
   - ② 削除して作り直す: 削除対象を提示して確認 → `DELETE` → 学習→テスト→保存。
   - ③ 別名で新規追加: 新しい `device_id` / `action` をユーザーと確定 → 学習→テスト→保存。
     既存デバイスは一切変更しない。
   - 類似名（「テレビ」と「リビングのテレビ」等）の場合も同じ3択で確認する。
   - **「登録し直して」「上書きして」「更新して」とユーザーが明示した場合**は
     ①上書きの事前了承とみなし、対象（device_id / label / action）を復唱してから学習を
     開始してよい。あいまいな「登録して」だけの場合は必ず3択を確認する。
   - 非対話環境で回答が得られない場合は、3択を提示して**停止**し、
     「何も変更していない」ことを報告する。
   - 重複がないと確認できた場合のみ、通常のワークフローへ進む。
3. **学習前のガイド（必ず出す）**
   次を1つのメッセージで伝える:
   - 押すボタン（例:「テレビのリモコンの『1』」）
   - Unit IR の**黒い受光部**へ向ける（30cm 以内）
   - **1回だけ**押す
   - 「これから約 N 秒間受付します（**N は実際の `timeout_ms` に合わせる**）」

   例文（`timeout_ms: 25000` の場合）:
   > ではテレビのリモコンの **「1」** を押してください。
   > M5Stack の画面に **`LEARN: living_tv/ch1`** と残り秒数が出たら受付中です。
   > 受光部（黒いドーム）へ向けて、**約25秒以内**に1回押してください。

   * 案内する秒数と `timeout_ms` を必ず一致させる（25000 → 約25秒、30000 → 約30秒）。
     実際より長い受付時間を案内しない。

4. **学習（受光待機）** — `device` / `action` を付けると画面に表示される
   ```bash
   curl -s -X POST "$M5IR_BASE_URL/api/ir/learn" \
     -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
     -d '{"timeout_ms": 25000, "device": "living_tv", "action": "ch1"}'
   ```
   * 既定 10 秒、最大 30 秒。HTTP クライアントのタイムアウトは `timeout_ms` より
     大きい値（目安 +5 秒、リレー経由はサーバー上限 35 秒）にする。
   * 応答例:
     `{"ok":true,"capture_id":"cap_8f2a","protocol":"NEC","bits":32,"raw_length":67}`
   * `capture_id` は 10 分で失効する。成功したらすぐ次へ進む。
   * 人間がリモコンを押せない自動実行では、`learn` を開始する前にその旨を報告する
     （成功したフリをしない）。
   * 受付状態は `GET /api/health` の `learn_target` / `learn_remaining_ms` でも確認できる
     （M5 画面と同じ情報）。
5. **失敗時の再ガイド（408 LEARN_TIMEOUT）**
   - 「押せましたか？受付時間内でしたか？」と確認する。
   - 具体的に再案内する（受光部の向き、距離、ボタン名、電池切れ、別のリモコン）。
   - `timeout_ms` を 30000 に延ばし（案内も「約30秒」に合わせる）、**1〜2回まで**再試行する。
   - それでも失敗したら、機材（Unit IR の接続・向き）を確認するよう案内する。
6. **テスト送信して実機の反応を確認**
   ```bash
   curl -s -X POST "$M5IR_BASE_URL/api/ir/test" \
     -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
     -d '{"capture_id":"cap_8f2a"}'
   ```
   * 「テレビは反応しましたか？（電源なら画面が消えた/点いた）」のように、
     何を見て判断すればよいかを具体的に聞く。
   * 反応しない場合は保存せず、**再学習を提案**する（LED を近づける・別ボタン）。
   * 複数アクションでも **各アクションごとに学習→テスト→登録**が基本。
   * 人間が確認できない自動実行では `sent:true` は「受理された」だけ。
     実機反応は未確認と報告する（`ir.backend="fake"` も同様）。
7. **永続保存（テスト成功後のみ）**
   ```bash
   curl -s -X POST "$M5IR_BASE_URL/api/devices/living_tv/actions" \
     -H "Authorization: Bearer $M5IR_API_KEY" -H "Content-Type: application/json" \
     -d '{"label":"リビングのテレビ","action":"ch1","capture_id":"cap_8f2a"}'
   ```
   * `device_id`: 英小文字・数字・アンダースコアのみ（1〜24 文字）。
   * `action`: 英小文字・数字・アンダースコアのみ（1〜32 文字）。
   * 新規デバイスの場合は `label` 必須。ユーザー指定の label を優先し、勝手に変えない。
   * 応答例:
     `{"ok":true,"device_id":"living_tv","action":"ch1","stored_format":"protocol"}`
   * **テスト成功前に保存しない**（Muse で devices が 0 のままだったのは、
     テスト送信が失敗してスキルどおり保存を見送ったため）。
8. **次のボタンへ（複数登録時）**
   保存後、次のボタンを具体的に案内してから次の `learn` を呼ぶ。
   例文:
   > 「1」を登録しました。次は **「2」** を押してください。
   > 画面に `LEARN: living_tv/ch2` と出たら受付中です。
9. **完了報告**
   `GET /api/devices` で最終確認し、登録した家電・アクション、テスト結果、
   次にできること（例:「テレビの電源も登録しますか？」）を簡潔に報告する。

## 操作間のインターバル（自分で判断して入れる）

API 呼び出しやユーザーへの案内の間には、**自分で適切な待機を設計して入れる**。
連続で叩き込むと家電が取りこぼしたり、ユーザーが操作に追いつけなくなる。

| 場面 | 目安 | 理由 |
| --- | --- | --- |
| 学習成功 → テスト送信 | 0.5〜1 秒 | 受信機・本体状態の復帰待ち |
| テスト送信 → 反応確認の質問 | 1〜2 秒 | 家電が反応する時間を確保 |
| 次のボタンの `learn` 開始まで | 1〜2 秒 ＋ 次ボタンの案内 | ユーザーがリモコンを持ち直す時間 |
| `408` 後の再試行まで | 1〜2 秒 | 再ガイドを読んでもらう時間 |
| `409 IR_BUSY` の再試行 | 1〜2 秒待って 1 回だけ | 排他の解除待ち |
| 単発送信を続けて行う場合 | 300〜500ms | 家電側のデコード間隔 |
| マクロ `steps` の `delay_ms` | 300〜1000ms（家電次第。AC は 1 秒程度） | 確実に反映させる |
| 保存 → 次の操作の案内 | 0.5〜1 秒 | 保存完了と画面更新 |

* 実装方法: シェルなら呼び出しの間に `sleep 1`、マクロは `steps` の `delay_ms` を使う。
* 追加の目安:
  * GET だけの確認（health / devices）は待機不要。
  * テスト送信に反応がない → 再学習まで 1〜2 秒。
  * テスト成功 → 保存まで 0.5〜1 秒。
  * DELETE → 学習まで 0.5〜1 秒。
  * リレー `504 DEVICE_TIMEOUT`（GET）→ 2〜3 秒待って最大 2 回まで再試行。
    Cloudflare の bot 判定 `403 (error 1010)` は端末の応答ではないため、`curl` で再試行してよい。
* 待機中も「少しお待ちください」など、ユーザーに状況を伝える。
* 待ちすぎは UX を悪化させる。**1〜2 秒を基本**に、家電の反応時間とユーザーの準備時間から
  自分で調整する。リレー経由のタイムアウト（通常 10 秒 / 学習 35 秒）は超えない。

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
4. **重複登録の確認（3択）**: 同じ家電・アクションが既にある、または同じ家電らしき
   別登録がある場合は、勝手に上書き・削除・新規追加をせず、**「①上書き更新 /
   ②削除して作り直す / ③別名で新規追加」の3択**を必ずユーザーに聞く。
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
