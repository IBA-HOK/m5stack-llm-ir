#!/usr/bin/env python3
"""Extract the shared Gemini Canvas specification into Markdown.

The shared page (share.gemini.google) is an Angular app. Rendering it with
headless Chromium produces a DOM where the actual document lives inside
`div.ProseMirror` (Gemini Canvas editor).  This script converts that HTML into
faithful Markdown (headings, lists, tables, code blocks, links).

Usage:
    python3 spec/extract_spec.py spec/share_dom.html spec/SPEC.md
"""

from __future__ import annotations

import sys
from html import unescape

from bs4 import BeautifulSoup, NavigableString, Tag


def cell_text(node: Tag) -> str:
    """Render a table cell as inline markdown, collapsing whitespace."""
    parts: list[str] = []

    def walk(n):
        if isinstance(n, NavigableString):
            parts.append(str(n))
            return
        name = n.name
        if name == "br":
            parts.append("<br>")
            return
        if name in ("strong", "b"):
            parts.append("**")
            walk_children(n)
            parts.append("**")
            return
        if name in ("em", "i"):
            parts.append("_")
            walk_children(n)
            parts.append("_")
            return
        if name == "code":
            parts.append("`")
            walk_children(n)
            parts.append("`")
            return
        if name == "a" and n.get("href"):
            parts.append("[")
            walk_children(n)
            parts.append(f"]({n['href']})")
            return
        walk_children(n)

    def walk_children(n):
        for c in n.children:
            walk(c)

    walk(node)
    text = "".join(parts)
    return " ".join(text.split()).replace("|", "\\|")


def convert_list(node: Tag, depth: int, ordered: bool, out: list[str]) -> None:
    indent = "  " * depth
    idx = 1
    for li in node.find_all("li", recursive=False):
        marker = f"{idx}." if ordered else "-"
        # Split paragraph-like children from nested lists.
        inline: list[str] = []
        nested: list[Tag] = []
        for child in li.children:
            if isinstance(child, Tag) and child.name in ("ul", "ol"):
                nested.append(child)
            else:
                inline.append(child)
        text = "".join(render_inline(c) for c in inline).strip()
        out.append(f"{indent}{marker} {text}")
        for sub in nested:
            convert_list(sub, depth + 1, sub.name == "ol", out)
        idx += 1


def render_inline(node) -> str:
    if isinstance(node, NavigableString):
        return str(node)
    name = node.name
    children = "".join(render_inline(c) for c in node.children)
    if name in ("strong", "b"):
        return f"**{children}**"
    if name in ("em", "i"):
        return f"_{children}_"
    if name == "code":
        return f"`{children}`"
    if name == "br":
        return "\n"
    if name == "a" and node.get("href"):
        return f"[{children}]({node['href']})"
    return children


def convert_block(node: Tag, out: list[str]) -> None:
    name = node.name
    if name in ("h1", "h2", "h3", "h4", "h5", "h6"):
        level = int(name[1])
        text = render_inline_text(node)
        out.append("")
        out.append("#" * level + " " + text)
        out.append("")
        return
    if name == "p":
        text = "".join(render_inline(c) for c in node.children).strip("\n")
        if text.strip():
            out.append(text)
            out.append("")
        return
    if name in ("ul", "ol"):
        convert_list(node, 0, name == "ol", out)
        out.append("")
        return
    if name == "pre":
        code = node.find("code")
        text = (code or node).get_text()
        lang = ""
        classes = (code or node).get("class") or []
        for c in classes:
            if c.startswith("language-"):
                lang = c[len("language-"):]
        out.append("")
        out.append(f"```{lang}")
        out.append(text.rstrip("\n"))
        out.append("```")
        out.append("")
        return
    if name == "table":
        render_table(node, out)
        return
    if name == "hr":
        out.extend(["", "---", ""])
        return
    if name == "blockquote":
        text = "".join(render_inline(c) for c in node.children).strip()
        out.extend(["> " + line for line in text.splitlines()])
        out.append("")
        return
    if isinstance(node, NavigableString):
        text = str(node).strip()
        if text:
            out.append(text)
            out.append("")
        return
    # generic container: recurse
    for child in node.children:
        if isinstance(child, Tag):
            convert_block(child, out)
        elif isinstance(child, NavigableString) and str(child).strip():
            out.append(str(child).strip())
            out.append("")


def render_inline_text(node: Tag) -> str:
    return " ".join("".join(render_inline(c) for c in node.children).split())


def render_table(table: Tag, out: list[str]) -> None:
    rows: list[list[str]] = []
    header_rows = 0
    for tr in table.find_all("tr"):
        cells = tr.find_all(["th", "td"], recursive=False)
        if not cells:
            continue
        rows.append([cell_text(c) for c in cells])
        if all(c.name == "th" for c in cells):
            header_rows += 1
    if not rows:
        return
    width = max(len(r) for r in rows)
    rows = [r + [""] * (width - len(r)) for r in rows]
    if header_rows == 0:
        # No real header: synthesize one from the first row.
        header = rows[0]
        body = rows[1:]
    else:
        header = rows[0]
        body = rows[1:]
    out.append("")
    out.append("| " + " | ".join(header) + " |")
    out.append("|" + "|".join(["---"] * width) + "|")
    for r in body:
        out.append("| " + " | ".join(r) + " |")
    out.append("")


def main(src: str, dst: str) -> None:
    html = open(src, encoding="utf-8", errors="replace").read()
    soup = BeautifulSoup(html, "lxml")

    editors = soup.select("div.ProseMirror")
    if not editors:
        raise SystemExit("No div.ProseMirror found; page rendering may have failed")
    # The largest ProseMirror block is the shared canvas document.
    editor = max(editors, key=lambda e: len(e.get_text(strip=False)))

    out: list[str] = []
    for child in editor.children:
        if isinstance(child, Tag):
            convert_block(child, out)
        elif isinstance(child, NavigableString) and str(child).strip():
            out.append(str(child).strip())
            out.append("")

    # Collapse excessive blank lines while keeping code blocks untouched.
    text = "\n".join(out)
    while "\n\n\n" in text:
        text = text.replace("\n\n\n", "\n\n")

    header = (
        "<!--\n"
        "  このファイルは共有された仕様書（Gemini Canvas）のローカルコピーです。\n"
        f"  取得元: https://share.gemini.google/QzSq4mluA4t0\n"
        "  取得方法: headless Chromium で描画した DOM の div.ProseMirror を Markdown へ変換\n"
        "  変換: spec/extract_spec.py\n"
        "-->\n\n"
    )
    with open(dst, "w", encoding="utf-8") as f:
        f.write(header + text.strip() + "\n")
    print(f"wrote {dst}: {len(text)} chars, {text.count(chr(10)) + 1} lines")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
