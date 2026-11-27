# 仕様書のローカルコピー

このディレクトリは、共有された仕様書（Gemini Canvas）のローカルコピーです。

| 項目 | 内容 |
| --- | --- |
| 共有 URL | <https://share.gemini.google/QzSq4mluA4t0> |
| 実体 URL | <https://gemini.google.com/share/3d816f14649a4124b7d9476f8e5fd54d> |
| 文書名 | M5Stack LLM スマートリモコン（MVP）仕様書 |
| バージョン | 0.2.0-draft |
| 最終更新日 | 2026-11-25 |
| 取得日 | 2026-11-26 |

## ファイル

| パス | 説明 |
| --- | --- |
| `SPEC.md` | 仕様書の Markdown コピー（**作業の正**） |
| `extract_spec.py` | 共有ページの描画 DOM から Markdown を再生成するスクリプト |
| `raw/` | 取得時の生データ（headless Chromium の DOM、原文 HTML、M5Stack 公式ドキュメントのハードウェア資料）。`.gitignore` 済み |

## 再取得手順

共有ページは Angular アプリで、素の HTTP 取得では本文が得られません。
headless Chromium で描画してから `div.ProseMirror`（Gemini Canvas の本文）を
Markdown へ変換しています。

```bash
# 1. headless Chromium で描画した DOM を保存（初回のみ）
CHROME="$HOME/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome"
"$CHROME" --headless=new --no-sandbox --disable-gpu --virtual-time-budget=25000 \
  --dump-dom "https://gemini.google.com/share/3d816f14649a4124b7d9476f8e5fd54d" \
  > spec/raw/share_dom.html

# 2. Markdown へ変換（BeautifulSoup が必要）
python3 -m venv .venv
.venv/bin/pip install beautifulsoup4 lxml
.venv/bin/python spec/extract_spec.py spec/raw/share_dom.html spec/SPEC.md
```

`SPEC.md` の先頭コメントに取得元と変換方法を記録しています。

> **注意**: `SPEC.md` の末尾には本リポジトリ独自の「付録A（ローカル追記）」
> （PORT.A 実験結果、高輝度 IR 送信機の検討など）を追加しています。
> `extract_spec.py` で再生成すると付録は消えるため、再適用するか
> `docs/` 配下へ分離してください。
