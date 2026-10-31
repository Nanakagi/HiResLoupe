"""MANUAL.md から MANUAL.html を作る。直し方は MANUAL.md だけ直して、このスクリプトを実行する。

    python tools/make_manual_html.py

MANUAL.md で使っている書き方（見出し・コードブロック・箇条書き・太字・コード・リンク・区切り線・<a id>）だけに対応した簡易版。
"""
import html
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
src = (root / "MANUAL.md").read_text(encoding="utf-8")

CSS = """
body{font-family:"Segoe UI","Yu Gothic UI","Meiryo",sans-serif;line-height:1.8;max-width:760px;margin:2em auto;padding:0 1em;color:#222;background:#fff}
h1{font-size:1.6em;border-bottom:2px solid #ccc;padding-bottom:.3em}
h2{font-size:1.3em;margin-top:2em;border-bottom:1px solid #ddd}
h3{font-size:1.1em;margin-top:1.6em}
code{background:#eee;border-radius:3px;padding:.1em .4em;font-family:Consolas,monospace}
ul{padding-left:1.4em}
pre{background:#eee;border-radius:4px;padding:.6em 1em;overflow-x:auto}pre code{background:none;padding:0}
pre{background:#eee;border-radius:4px;padding:.6em 1em;overflow-x:auto}pre code{background:none;padding:0}
hr{border:0;border-top:1px solid #ccc;margin:2em 0}
a{color:#0b5cad}
@media (prefers-color-scheme:dark){body{color:#ddd;background:#1e1e1e}h1,h2{border-color:#444}code,pre{background:#333}a{color:#6cb2ff}hr{border-color:#444}}
"""


def inline(text):
    parts = re.split(r"(`[^`]+`)", text)
    out = []
    for p in parts:
        if p.startswith("`") and p.endswith("`") and len(p) > 1:
            out.append("<code>%s</code>" % html.escape(p[1:-1]))
            continue
        p = html.escape(p, quote=False)
        p = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", p)
        p = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', p)
        out.append(p)
    return "".join(out)


body = []
in_list = False
para = []


def flush_para():
    global para
    if para:
        body.append("<p>%s</p>" % "<br>\n".join(inline(l) for l in para))
        para = []


def close_list():
    global in_list
    if in_list:
        body.append("</ul>")
        in_list = False


in_code = False
code = []
for line in src.split("\n"):
    s = line.rstrip()
    if s.startswith("```"):
        if in_code:
            body.append("<pre><code>%s</code></pre>" % html.escape("\n".join(code), quote=False))
            code = []
        else:
            flush_para(); close_list()
        in_code = not in_code
        continue
    if in_code:
        code.append(line.rstrip("\r"))
        continue
    m = re.match(r"^(#{1,3}) (.*)$", s)
    if m:
        flush_para(); close_list()
        n = len(m.group(1))
        body.append("<h%d>%s</h%d>" % (n, inline(m.group(2)), n))
    elif s.startswith("- "):
        flush_para()
        if not in_list:
            body.append("<ul>")
            in_list = True
        body.append("<li>%s</li>" % inline(s[2:]))
    elif s == "---":
        flush_para(); close_list()
        body.append("<hr>")
    elif s.startswith("<a id="):
        flush_para(); close_list()
        body.append(s)
    elif s == "":
        flush_para(); close_list()
    else:
        close_list()
        para.append(s)
flush_para(); close_list()

doc = (
    '<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
    '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
    "<title>HiResLoupe Manual / 高解像ルーペ 使い方</title>\n<style>%s</style>\n</head>\n<body>\n%s\n</body>\n</html>\n"
    % (CSS, "\n".join(body))
)
(root / "MANUAL.html").write_text(doc, encoding="utf-8", newline="\n")
print("OK: MANUAL.html", file=sys.stderr)
