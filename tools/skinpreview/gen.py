#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FBUI 皮肤布局预览生成器。

读皮肤包的 layout.ini 和 bg.png，连同两套点阵字形一起塞进一个自包含的 HTML。
生成出来的文件双击就能开，不需要服务器，也不需要真机。

它能回答的问题：
  - 240p 下汉字到底糊不糊，横向倍宽有没有救回来（可以开关对比）
  - 画面有没有超出 CRT 过扫描安全区
  - 列表能放几行、面板框多大、缩略图按固定尺寸贴上去变不变形

它回答不了的问题：
  - 真实 CRT 的扫描线、色偏、几何失真
  - 缩略图解码耗时

关键点是**显示像素宽高比**：640x240 在 4:3 屏上，一个帧缓冲像素的高度是宽度的
两倍（PAR 1:2）。所以预览默认把 240p 画布横向拉伸两倍显示，这才是眼睛看到的样子；
想看帧缓冲里像素的真实排布可以切到「原始像素」。

用法::

    python gen.py                     # 用 skins/default，输出 preview.html
    python gen.py --skin skins/mytheme --unifont /tmp/unifont.hex
"""

import argparse
import base64
import json
import os
import re
import sys
import urllib.request

SELF = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(SELF, "..", ".."))

UNIFONT_URL = ("https://unifoundry.com/pub/unifont/unifont-15.1.05/"
               "font-builds/unifont-15.1.05.hex.gz")

# 预览里会出现的文字。只有这些码点的字形会被打进 HTML，所以文件很小。
# 改了这里要重新生成。
SAMPLE = {
    "title_sys":  "游戏库 GAME LIBRARY",
    "title_game": "Super Nintendo / 角色扮演",
    "hint_sys":   "A 进入   START 菜单",
    "hint_game":  "A 启动   B 返回   START 菜单",
    "hint_sys_hd":  "ENTER 进入   F12/ESC 经典菜单",
    "hint_game_hd": "ENTER 启动   BACKSPACE 返回   F12/ESC 经典菜单",
    "empty": "（空目录）",
}

ROWS_SYS = [
    "> Nintendo Entertainment System",
    "> Super Nintendo",
    "> 世嘉 Mega Drive",
    "> PlayStation",
    "> Neo Geo 街机",
    "> Game Boy Advance",
    "> TurboGrafx-16",
    "> Atari 2600",
    "> 街机 Arcade",
    "> Commodore 64",
    "> 世嘉 Master System",
    "> Nintendo 64",
]

ROWS_GAME = [
    "超级马里奥世界",
    "塞尔达传说 众神的三角力量",
    "热血物语",
    "Chrono Trigger",
    "Final Fantasy VI",
    "口袋妖怪 红宝石",
    "魂斗罗 精神续作",
    "Donkey Kong Country 2",
    "街头霸王 II 特别版",
    "恶魔城 晓月圆舞曲",
    "Secret of Mana",
    "火焰纹章 圣战的系谱",
]


# ---------------------------------------------------------------------------
# 字形
# ---------------------------------------------------------------------------

def load_charfont(path):
    """从 charrom.cpp 里抠出 MiSTer 内建的 8x8 点阵（列优先，bit0 在上）。

    240p 的 ASCII 走的就是这套字面（等比放大两倍到 16x16 格子），不是 unifont，
    所以预览必须用同一份数据，不能拿别的 8x8 字体顶替。
    """
    src = open(path, encoding="utf-8", errors="replace").read()
    i = src.index("charfont[256][8]")
    body = src[src.index("{", i) + 1:]
    rows = re.findall(r"\{([^{}]*)\}", body)
    out = []
    for r in rows[:256]:
        vals = [int(v, 0) for v in re.findall(r"0[xX][0-9a-fA-F]+|\d+", r)]
        if len(vals) != 8:
            break
        out.append(vals)
    if len(out) < 128:
        raise SystemExit("charrom.cpp 里没解析出完整的 charfont")
    return out[:128]


def fetch_unifont(dst):
    import gzip
    print("下载 GNU Unifont ...")
    with urllib.request.urlopen(UNIFONT_URL, timeout=180) as r:
        data = gzip.decompress(r.read())
    open(dst, "wb").write(data)
    return dst


def load_unifont(path, codepoints):
    """只取需要的码点。返回 {码点: {"w": 8|16, "rows": [每行位图整数]}}。"""
    want = set(codepoints)
    out = {}
    with open(path, encoding="latin-1") as f:
        for line in f:
            k, _, v = line.partition(":")
            if not v:
                continue
            try:
                cp = int(k, 16)
            except ValueError:
                continue
            if cp not in want:
                continue
            v = v.strip()
            # 32 个十六进制字符 = 8 宽，64 个 = 16 宽
            if len(v) == 32:
                out[cp] = {"w": 8, "rows": [int(v[i:i + 2], 16) for i in range(0, 32, 2)]}
            elif len(v) == 64:
                out[cp] = {"w": 16, "rows": [int(v[i:i + 4], 16) for i in range(0, 64, 4)]}
            if len(out) == len(want):
                break
    return out


# ---------------------------------------------------------------------------
# 皮肤包
# ---------------------------------------------------------------------------

def parse_layout(path):
    L = {}
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        k, _, v = line.partition("=")
        k, v = k.strip(), v.strip()
        parts = [p.strip() for p in v.split(",")]
        if len(parts) > 1:
            L[k] = [int(p, 0) for p in parts]
        elif re.fullmatch(r"0[xX][0-9a-fA-F]+|-?\d+", v):
            L[k] = int(v, 0)
        else:
            L[k] = v
    return L


def collect_template_cjk(path):
    """扫模板里画在画布上的文案。

    只认 drawText/uiText 的字符串实参 —— 页面本身的中文界面用的是系统字体，
    不需要点阵字形，全收进来只会让文件变大。
    """
    src = open(path, encoding="utf-8").read()
    out = set()
    for m in re.finditer(r'(?:drawText|uiText)\s*\([^;]*?"([^"]*)"', src):
        out |= {ord(c) for c in m.group(1)}
    for m in re.finditer(r'const\s+cap\s*=\s*"([^"]*)"', src):
        out |= {ord(c) for c in m.group(1)}
    return out


def read_profile(skin_dir, prof):
    d = os.path.join(skin_dir, prof)
    ini = os.path.join(d, "layout.ini")
    if not os.path.isfile(ini):
        return None
    L = parse_layout(ini)
    bg = os.path.join(d, "bg.png")
    if os.path.isfile(bg):
        L["bg_data"] = "data:image/png;base64," + \
            base64.b64encode(open(bg, "rb").read()).decode("ascii")
    return L


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="生成 FBUI 皮肤布局预览网页")
    ap.add_argument("--skin", default=os.path.join(ROOT, "skins", "default"))
    ap.add_argument("--unifont", help="unifont.hex 路径；不给则下载并缓存到本目录")
    ap.add_argument("--out", default=os.path.join(SELF, "preview.html"))
    args = ap.parse_args()

    texts = list(SAMPLE.values()) + ROWS_SYS + ROWS_GAME + \
        ["0123456789/ ", "1 / 12", "12 / 12"]
    codepoints = {ord(c) for t in texts for c in t}
    # 模板里还有几处写死在 JS 字面量里的文案（缩略图占位说明之类）。漏掉它们
    # 会在页面上显示成空心方框，所以这里连模板一起扫。
    codepoints |= collect_template_cjk(os.path.join(SELF, "template.html"))
    codepoints = sorted(codepoints)

    uni_path = args.unifont
    if not uni_path:
        cached = os.path.join(SELF, "unifont.hex")
        for cand in (cached, "/tmp/unifont.hex"):
            if os.path.isfile(cand):
                uni_path = cand
                break
        else:
            uni_path = fetch_unifont(cached)
    glyphs = load_unifont(uni_path, codepoints)
    missing = [cp for cp in codepoints if cp not in glyphs]
    if missing:
        print("unifont 缺字 %d 个: %s" % (
            len(missing), " ".join("U+%04X" % c for c in missing[:10])))

    # layout.ini 里没有画布尺寸（引擎从视频模式拿），这里按档位补上
    SIZES = {"240p": (640, 240), "1080p": (1920, 1080)}

    profiles = {}
    for prof in ("240p", "1080p"):
        L = read_profile(args.skin, prof)
        if not L:
            continue
        L["w"], L["h"] = SIZES[prof]
        profiles[prof] = L
        print("%s: %dx%d, 列表 %d 行, 面板 %dx%d" % (
            prof, L["w"], L["h"], L["list"][3] // L["row_h"],
            L["panel"][2], L["panel"][3]))
    if not profiles:
        raise SystemExit("皮肤包里没有可用的 layout.ini: " + args.skin)

    data = {
        "skin": os.path.basename(os.path.normpath(args.skin)),
        "profiles": profiles,
        "charfont": load_charfont(os.path.join(ROOT, "charrom.cpp")),
        "glyphs": {str(k): v for k, v in glyphs.items()},
        "sample": SAMPLE,
        "rows_sys": ROWS_SYS,
        "rows_game": ROWS_GAME,
    }

    tpl = open(os.path.join(SELF, "template.html"), encoding="utf-8").read()
    html = tpl.replace("/*__DATA__*/null",
                       json.dumps(data, ensure_ascii=False, separators=(",", ":")))
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print("生成 %s (%.0f KiB)" % (args.out, os.path.getsize(args.out) / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
