# スキル検証レポート（m5ir-remote）

「**スキル文書（[`skill/SKILL.md`](../skill/SKILL.md)）だけで、LLM エージェントが正しく指示・操作できるか**」を、
会話コンテキストを一切与えない新規サブエージェントで検証した記録です。

## 検証方法

- 親エージェントの会話履歴を引き継がない **新規サブエージェント**を起動（`subagent`、fork ではない）。
- サブエージェントに渡すのは次の 3 点のみ:
  1. `skill/SKILL.md` を読んで従うこと
  2. `M5IR_BASE_URL`（Cloudflare Worker の公開 URL）と、API キーの取得先（`.env`。値の表示は禁止）
  3. ユーザーからの自然言語の依頼（下記シナリオ）
- 実 HTTP 呼び出しで実機（Cloudflare リレー経由の M5Stack）を操作させ、実行ログ・最終状態・
  スキルへの自己評価を報告させる。
- API キーは各サブエージェントが `.env` からシェル変数として読み、**出力・報告には含めない**ことを確認。

## 実施した 3 回のテスト

| # | デバイスの IR 状態 | 依頼内容 | 結果 |
| --- | --- | --- | --- |
| 1 | `backend="fake"`（試験用合成 IR） | 学習→テスト→登録×2→単発送信→バッチ送信→削除 | **フローは完遂**。ただし `ir.backend="fake"` をスキルが扱えず「実機で反応した」と誤認するリスクを指摘 |
| 2 | `backend="fake"`（スキル修正後） | 同上 | **合格**。`fake` を「試験用の合成バックエンド」と正しく解釈し、実機反応の断定を避けて報告。削除まで確認 |
| 3 | `backend="stub"`（Unit IR 未接続） | 「テレビの power を学習して登録しておいて」 | **合格**。`learn` が 503 `IR_NOT_AVAILABLE` を返すと正しく解釈し、**登録せず**、未接続の理由と復旧手順をユーザーへ説明 |

### テスト 2 で実際に行われた API 呼び出し（概要）

```
GET  /api/health                         200  ir.hardware=true, ir.backend="fake"
GET  /api/devices                        200  {}
POST /api/ir/learn                       200  capture_id=cap_deb5 (NEC/32bit)
POST /api/ir/test                        200  sent=true
POST /api/devices/skill_test_tv2/actions 201  stored_format=protocol
POST /api/ir/send                        200  executed_steps=1
POST /api/ir/learn                       200  capture_id=cap_72a2
POST /api/devices/skill_test_tv2/actions 201  stored_format=protocol
GET  /api/devices                        200  actions=["power","vol_up"]
POST /api/ir/send (steps)                200  executed_steps=3
DELETE /api/devices/skill_test_tv2       200
GET  /api/devices                        200  {}
```

テスト後の最終状態: `devices={}`, `state=idle`, `ir.backend="stub"`（本番ファームウェアに戻し済み）。

## テストで見つかった不足と、スキルへの反映

| # | 指摘 | 対応 |
| --- | --- | --- |
| 1 | `ir.backend="fake"`（合成 IR）の扱いが未定義。実機動作と誤認するリスク | health 判定を `ir.hardware` + `ir.backend` の 3 状態（`irremote` / `stub` / `fake`）で説明。`fake` は「試験用」「実機反応として報告しない」と明記 |
| 2 | 非対話実行時の「テレビが反応しましたか？」確認の扱いが不明 | `sent:true` は「API 受理」に留め、実機反応は未確認と報告するルールを追加 |
| 3 | 事前了承済みの削除の扱いが不明 | 同一リクエスト内で事前了承があれば対象を復唱して実行可、削除後は一覧再確認、と明記 |
| 4 | タイムアウトの「+5 秒」が下限か上限か曖昧 | 「`timeout_ms` より大きい値（目安 +5 秒）、リレー経由の上限 35 秒、`timeout_ms` は 30 秒以下」に整理 |
| 5 | `executed_steps` の数え方（`delay_ms` を含むか）が未定義 | 「`steps` 要素数（送信 + delay の合計）」と例を追記 |
| 6 | `DELETE` の成功ステータスが未記載 | `DELETE` は `200` + JSON ボディ、アクション保存は `201` と明記 |
| 7 | current `capture_id` 形式の例が実測と不一致 | `cap_` + 4 桁 16 進の例に更新 |
| 8 | 各アクションのテスト送信要否が不明 | 「各アクションごとに学習→テスト→登録が基本（省略はユーザー明示時のみ）」と追記 |
| 9 | `stub` 時に `learn` を呼ぶかどうか不明 | 「health 時点で説明してよい／確認目的で 1 回試すのも可／登録・送信・架空 capture は禁止」と追記 |
| 10 | 自動実行で人間がリモコンを押せない場合の手順 | 「待機前にその旨を報告し、成功させない」と追記 |

## 結論

- 修正後のスキル文書は、**会話コンテキストなしのサブエージェントでも**、
  - 正しいエンドポイント・認証・タイムアウトの選択
  - 学習→テスト→登録→送信→削除のワークフロー
  - `fake` / `stub` / `irremote` のバックエンド判定と正直な報告
  - 推測禁止・削除前確認・削除後検証
  を一貫して実行できることを確認しました（テスト 2・3: 合格）。
- 残課題: 非対話実行時の「学習を試すか／試さないか」は運用判断の幅を残しています
  （どちらも許容する記述にした）。将来的にはバックエンド別の判断フロー図を追加するとより明確になります。

## 再現方法

親エージェントのコンテキストを渡さず、以下の骨子で新規サブエージェントを起動します。

```text
あなたは自律型 LLM エージェントです。会話履歴はありません。
スキル: <repo>/skill/SKILL.md を読み、その手順・安全ルールに従うこと。
接続: M5IR_BASE_URL=<worker URL> / API キーは <repo>/.env の M5IR_API_KEY（値は表示しない）
依頼: 「リビングのテレビ（device_id: skill_test_tv）の power を学習して登録して…」
要件: 実 HTTP で操作し、各呼び出しの status・結果・最終状態・スキルの不足点を報告する。
```

> 検証時の生ログ（JSON）はローカルの `.e2e/`（git-ignored）に保存されています。
