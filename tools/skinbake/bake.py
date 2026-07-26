#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FBUI 皮肤离线烘焙工具。

把美术素材（EmulationStation 主题包里的 SVG/JPG，或自制稿）一次性转成
FBUI 运行时直接可用的定尺寸 PNG，并生成 layout.ini。

设计前提：运行时不做任何缩放和光栅化，所有变形都在这里做完。
240p (640x240) 的帧缓冲像素宽度只有高度的一半（PAR 1:2），所以同一张原图
在 240p 和 1080p 下要输出两张尺寸完全不同、且不成等比关系的 PNG。

用法::

    # 只生成两套 layout.ini（不需要任何第三方库）
    python bake.py --out /media/fat/skins/default

    # 连同 ES 主题包里的系统 logo 和背景一起烘焙
    python bake.py --theme ~/themes/es-theme-carbon --out ./skins/default

    # 只出 240p 那一套
    python bake.py --theme ... --out ... --only 240p

依赖：
    图片转换需要 Pillow（pip install pillow）。
    SVG 需要 cairosvg（pip install cairosvg），或系统里有
    rsvg-convert / inkscape / ImageMagick 之一。
    只生成 layout.ini 时（不加 --theme/--bg）不需要任何依赖。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

# ---------------------------------------------------------------------------
# 分辨率档位
# ---------------------------------------------------------------------------

# par 是显示像素宽高比 (宽, 高)。640x240 在 4:3 屏上是 1:2。
PROFILES = {
    "240p":  {"w": 640,  "h": 240,  "par": (1, 2)},
    "1080p": {"w": 1920, "h": 1080, "par": (1, 1)},
}

DEFAULT_COLORS = {
    "col_bg_top":   0x1A2029,
    "col_bg_bot":   0x0D1015,
    "col_header":   0x232B38,
    "col_panel":    0x161B24,
    "col_text":     0xC8D0DA,
    "col_dim":      0x707A88,
    "col_sel_bar":  0x2A6BD8,
    "col_sel_text": 0xFFFFFF,
    "col_accent":   0x4A90E2,
    "col_title":    0xE0E0E0,
    "col_msg":      0xFFB040,
}


def compute_layout(name):
    """复刻 fbui_skin.cpp 里 skin_defaults() 的几何推导。

    生成的 layout.ini 必须和内建默认值逐个数字一致，这样皮肤作者拿到的是一份
    「和当前观感完全相同」的起点，改哪一项就只影响哪一项。
    """
    p = PROFILES[name]
    w, h = p["w"], p["h"]
    lowres = h <= 288
    scale = 2 if h >= 900 else 1

    L = dict(DEFAULT_COLORS)
    L.update(w=w, h=h, lowres=lowres, scale=scale, par=p["par"])

    if lowres:
        L["safe_l"] = L["safe_r"] = w * 6 // 100
        L["safe_t"] = L["safe_b"] = h * 6 // 100
        L["header_h"] = 24
        L["footer_h"] = 24
        L["row_h"] = 22
        L["row_pad"] = 4
        L["scrollbar_w"] = 3
        L["list_x"] = L["safe_l"] + 8
        L["list_y"] = L["safe_t"] + L["header_h"] + 4
        L["list_w"] = (w - L["safe_l"] - L["safe_r"]) * 58 // 100 - 8
        L["panel_x"] = L["safe_l"] + (w - L["safe_l"] - L["safe_r"]) * 60 // 100
        L["panel_y"] = L["list_y"]
        L["panel_w"] = w - L["safe_r"] - L["panel_x"] - 8
        L["panel_h"] = h - L["safe_b"] - L["footer_h"] - 4 - L["panel_y"]
        L["font_px"] = 16
        L["title_px"] = 16
        L["footer_px"] = 16
        L["cache_mb"] = 48
    else:
        s = scale
        L["safe_l"] = L["safe_t"] = L["safe_r"] = L["safe_b"] = 0
        L["header_h"] = 28 * s
        L["footer_h"] = 24 * s
        L["row_h"] = 20 * s
        L["row_pad"] = 8 * s
        L["scrollbar_w"] = 3 * s
        L["list_x"] = 12 * s
        L["list_y"] = L["header_h"] + 8 * s
        L["list_w"] = w * 58 // 100 - 12 * s
        L["panel_x"] = w * 60 // 100
        L["panel_y"] = L["list_y"]
        L["panel_w"] = w - L["panel_x"] - 12 * s
        L["panel_h"] = h - L["panel_y"] - L["footer_h"] - 8 * s
        L["font_px"] = 16 * s
        L["title_px"] = L["font_px"] + L["font_px"] // 3
        L["footer_px"] = L["font_px"] * 4 // 5
        L["cache_mb"] = 128

    # 列表高度：引擎按 高度 / 行高 反推可见行数
    L["list_h"] = h - L["safe_b"] - L["footer_h"] - 4 - L["list_y"]
    L["list_rows"] = max(1, L["list_h"] // L["row_h"])

    # showcase 的 logo 框
    sw = w - L["safe_l"] - L["safe_r"]
    sh = h - L["safe_t"] - L["safe_b"]
    L["logo_x"] = L["safe_l"] + sw * 10 // 100
    L["logo_y"] = L["safe_t"] + sh * 18 // 100
    L["logo_w"] = sw * 80 // 100
    L["logo_h"] = sh * 40 // 100

    # 固定尺寸资源槽就是面板框本身
    L["thumb_w"] = L["panel_w"]
    L["thumb_h"] = L["panel_h"]
    L["sysbg_w"] = w if lowres else 960
    L["sysbg_h"] = h if lowres else 540
    L["syslogo_w"] = L["logo_w"]
    L["syslogo_h"] = L["logo_h"]
    return L


def write_layout_ini(path, L, showcase):
    lines = [
        "# 由 tools/skinbake/bake.py 生成。坐标单位是帧缓冲像素，运行时不做缩放。",
        "# 目标分辨率 %dx%d，显示像素宽高比 %d:%d。" % (L["w"], L["h"], L["par"][0], L["par"][1]),
        "",
        "showcase=%d" % (1 if showcase else 0),
        "par=%d,%d" % L["par"],
        "",
        "# 过扫描安全区内缩：左,上,右,下",
        "safe=%d,%d,%d,%d" % (L["safe_l"], L["safe_t"], L["safe_r"], L["safe_b"]),
        "header_h=%d" % L["header_h"],
        "footer_h=%d" % L["footer_h"],
        "",
        "# 列表框 x,y,宽,高。可见行数 = 高 / row_h（这里是 %d 行）" % L["list_rows"],
        "list=%d,%d,%d,%d" % (L["list_x"], L["list_y"], L["list_w"], L["list_h"]),
        "row_h=%d" % L["row_h"],
        "row_pad=%d" % L["row_pad"],
        "scrollbar_w=%d" % L["scrollbar_w"],
        "list_align=left",
        "",
        "# 媒体面板 x,y,宽,高。系统层放 logo，游戏层放封面，位置不随层级变。",
        "panel=%d,%d,%d,%d" % (L["panel_x"], L["panel_y"], L["panel_w"], L["panel_h"]),
        "",
        "# showcase 版式的系统 logo 框",
        "logo=%d,%d,%d,%d" % (L["logo_x"], L["logo_y"], L["logo_w"], L["logo_h"]),
        "",
        "# 缩略图槽位固定尺寸，缓存预算按它反推槽位数",
        "thumb=%d,%d" % (L["thumb_w"], L["thumb_h"]),
        "sysbg=%d,%d" % (L["sysbg_w"], L["sysbg_h"]),
        "cache_mb=%d" % L["cache_mb"],
        "",
        "# 文字高度",
        "font_px=%d" % L["font_px"],
        "title_px=%d" % L["title_px"],
        "footer_px=%d" % L["footer_px"],
        "",
        "# 颜色 0xRRGGBB",
    ]
    for k in sorted(DEFAULT_COLORS):
        lines.append("%s=0x%06X" % (k, L[k]))
    lines.append("")

    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


# ---------------------------------------------------------------------------
# 图像转换
# ---------------------------------------------------------------------------

def need_pillow():
    try:
        from PIL import Image  # noqa: F401
        return True
    except ImportError:
        sys.stderr.write("错误：图片烘焙需要 Pillow，请先 pip install pillow\n")
        return False


def visual_aspect(w, h, par):
    """一块 w x h 的帧缓冲区域在屏幕上看起来的宽高比。"""
    return (w * par[0]) / float(h * par[1])


def fit_size(sw, sh, box_w, box_h, par):
    """求帧缓冲尺寸，使图片显示出来不变形且不超出 box。

    要求 (dw * par_w) / (dh * par_h) == sw / sh。
    """
    num = sw * par[1]
    den = sh * par[0]
    dw = box_w
    dh = max(1, int(round(dw * den / float(num))))
    if dh > box_h:
        dh = box_h
        dw = max(1, int(round(dh * num / float(den))))
    return max(1, min(dw, box_w)), max(1, dh)


def rasterize_svg(src, w, h):
    """把 SVG 光栅化成 Pillow Image。找不到任何后端时返回 None。"""
    from PIL import Image
    import io

    try:
        import cairosvg
        png = cairosvg.svg2png(url=src, output_width=w, output_height=h)
        return Image.open(io.BytesIO(png)).convert("RGBA")
    except ImportError:
        pass
    except Exception as e:
        sys.stderr.write("  cairosvg 失败 %s: %s\n" % (src, e))

    for cmd in (
        ["rsvg-convert", "-w", str(w), "-h", str(h), "-o", "-", src],
        ["inkscape", "--export-type=png", "--export-filename=-",
         "-w", str(w), "-h", str(h), src],
        ["magick", "-background", "none", "-density", "300", src,
         "-resize", "%dx%d" % (w, h), "png:-"],
        ["convert", "-background", "none", "-density", "300", src,
         "-resize", "%dx%d" % (w, h), "png:-"],
    ):
        if not shutil.which(cmd[0]):
            continue
        try:
            out = subprocess.run(cmd, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, check=True).stdout
            if out:
                return Image.open(io.BytesIO(out)).convert("RGBA")
        except Exception:
            continue
    return None


def load_source(src, hint_w, hint_h):
    from PIL import Image
    if src.lower().endswith(".svg"):
        # SVG 先按目标尺寸的两倍光栅化再缩，边缘更干净
        return rasterize_svg(src, hint_w * 2, hint_h * 2)
    try:
        return Image.open(src).convert("RGBA")
    except Exception as e:
        sys.stderr.write("  打不开 %s: %s\n" % (src, e))
        return None


def bake_fit(src, dst, box_w, box_h, par):
    """等比（视觉上）缩到 box 以内，输出带 alpha 的 PNG。用于 logo。"""
    from PIL import Image
    img = load_source(src, box_w, box_h)
    if img is None:
        return False
    dw, dh = fit_size(img.width, img.height, box_w, box_h, par)
    img = img.resize((dw, dh), Image.LANCZOS)
    img.save(dst, "PNG", optimize=True)
    return True


def bake_cover(src, dst, out_w, out_h, par):
    """铺满 out_w x out_h：先按屏幕的视觉宽高比中心裁剪，再缩到目标尺寸。

    运行时会把这张图直接拉伸到整屏，所以这里必须把 PAR 变形一并做掉。
    """
    from PIL import Image
    img = load_source(src, out_w, out_h)
    if img is None:
        return False

    want = visual_aspect(out_w, out_h, par)
    have = img.width / float(img.height)
    if have > want:
        cw = int(round(img.height * want))
        x = (img.width - cw) // 2
        img = img.crop((x, 0, x + cw, img.height))
    elif have < want:
        ch = int(round(img.width / want))
        y = (img.height - ch) // 2
        img = img.crop((0, y, img.width, y + ch))

    img = img.resize((out_w, out_h), Image.LANCZOS)
    img.convert("RGB").save(dst, "PNG", optimize=True)
    return True


# ---------------------------------------------------------------------------
# ES 主题包扫描
# ---------------------------------------------------------------------------

# Carbon 是 <es>/art/system.svg，Atlas 是 assets/systems/logos/<es>.svg
LOGO_PATTERNS = [
    "assets/systems/logos/{es}-w.svg",
    "assets/systems/logos/{es}-w.png",
    "assets/systems/logos/{es}.svg",
    "assets/systems/logos/{es}.png",
    "{es}/art/system.svg",
    "{es}/art/system.png",
]
BG_PATTERNS = [
    "assets/systems/background/{es}.jpg",
    "assets/systems/background/{es}.png",
    "{es}/art/background.jpg",
    "{es}/art/background.png",
]


def discover_systems(theme_dir):
    """列出主题包里出现过的所有 ES 系统短名。"""
    names = set()

    for sub in ("assets/systems/logos", "assets/systems/background"):
        d = os.path.join(theme_dir, sub)
        if not os.path.isdir(d):
            continue
        for fn in os.listdir(d):
            stem, ext = os.path.splitext(fn)
            if ext.lower() not in (".svg", ".png", ".jpg", ".jpeg"):
                continue
            names.add(re.sub(r"-w$", "", stem))

    # 每系统一个子目录的老式布局
    for fn in os.listdir(theme_dir):
        d = os.path.join(theme_dir, fn)
        if os.path.isdir(d) and os.path.isdir(os.path.join(d, "art")):
            names.add(fn)

    return sorted(names)


def first_existing(theme_dir, patterns, es):
    for pat in patterns:
        p = os.path.join(theme_dir, pat.format(es=es))
        if os.path.isfile(p):
            return p
    return None


def bake_systems(theme_dir, out_dir, L):
    systems = discover_systems(theme_dir)
    if not systems:
        print("  主题包里没找到任何系统资源")
        return

    logo_dir = os.path.join(out_dir, "systems")
    bg_dir = os.path.join(out_dir, "sysbg")
    os.makedirs(logo_dir, exist_ok=True)
    os.makedirs(bg_dir, exist_ok=True)

    n_logo = n_bg = 0
    for es in systems:
        src = first_existing(theme_dir, LOGO_PATTERNS, es)
        if src and bake_fit(src, os.path.join(logo_dir, es + ".png"),
                            L["syslogo_w"], L["syslogo_h"], L["par"]):
            n_logo += 1
        src = first_existing(theme_dir, BG_PATTERNS, es)
        if src and bake_cover(src, os.path.join(bg_dir, es + ".png"),
                              L["sysbg_w"], L["sysbg_h"], L["par"]):
            n_bg += 1

    print("  系统 logo %d 张 -> %s" % (n_logo, logo_dir))
    print("  系统背景 %d 张 -> %s" % (n_bg, bg_dir))


# ---------------------------------------------------------------------------
# 静态层合成
# ---------------------------------------------------------------------------

def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def compose_bg(dst, L, base_img):
    """把整屏静态层合成成一张 bg.png：底图/渐变 + 上下条带 + 面板底框。

    运行时解码一次就是 bglayer，所有动态绘制都只在它上面做局部还原。
    """
    from PIL import Image, ImageDraw

    w, h = L["w"], L["h"]
    if base_img:
        img = load_source(base_img, w, h)
        if img is None:
            return False
        want = visual_aspect(w, h, L["par"])
        have = img.width / float(img.height)
        if have > want:
            cw = int(round(img.height * want))
            x = (img.width - cw) // 2
            img = img.crop((x, 0, x + cw, img.height))
        elif have < want:
            ch = int(round(img.width / want))
            y = (img.height - ch) // 2
            img = img.crop((0, y, img.width, y + ch))
        img = img.resize((w, h), Image.LANCZOS).convert("RGB")
    else:
        img = Image.new("RGB", (w, h))
        d = ImageDraw.Draw(img)
        r0, g0, b0 = rgb(L["col_bg_top"])
        r1, g1, b1 = rgb(L["col_bg_bot"])
        for y in range(h):
            d.line([(0, y), (w, y)], fill=(r0 + (r1 - r0) * y // h,
                                           g0 + (g1 - g0) * y // h,
                                           b0 + (b1 - b0) * y // h))

    d = ImageDraw.Draw(img)
    bx = L["safe_l"]
    bw = w - L["safe_l"] - L["safe_r"]
    accent_h = 2 * L["scale"]

    d.rectangle([bx, L["safe_t"], bx + bw - 1, L["safe_t"] + L["header_h"] - 1],
                fill=rgb(L["col_header"]))
    d.rectangle([bx, L["safe_t"] + L["header_h"] - accent_h,
                 bx + bw - 1, L["safe_t"] + L["header_h"] - 1],
                fill=rgb(L["col_accent"]))
    fy = h - L["safe_b"] - L["footer_h"]
    d.rectangle([bx, fy, bx + bw - 1, fy + L["footer_h"] - 1], fill=rgb(L["col_header"]))
    d.rectangle([L["panel_x"], L["panel_y"],
                 L["panel_x"] + L["panel_w"] - 1, L["panel_y"] + L["panel_h"] - 1],
                fill=rgb(L["col_panel"]))
    d.rectangle([L["panel_x"], L["panel_y"],
                 L["panel_x"] + L["panel_w"] - 1, L["panel_y"] + accent_h - 1],
                fill=rgb(L["col_accent"]))

    img.save(dst, "PNG", optimize=True)
    return True


# ---------------------------------------------------------------------------

def write_skin_ini(path, name, font):
    lines = [
        "# FBUI 皮肤包。运行时按开机选定的分辨率只加载其中一个子目录。",
        "name=%s" % name,
    ]
    if font:
        lines.append("# 相对路径按 SD 卡根目录解析；240p 恒用内建位图字体，此项只影响 HD")
        lines.append("font=%s" % font)
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(
        description="FBUI 皮肤烘焙工具：把美术素材转成定尺寸 PNG 并生成 layout.ini")
    ap.add_argument("--out", required=True, help="输出的皮肤包目录，例如 /media/fat/skins/default")
    ap.add_argument("--theme", help="EmulationStation 主题包目录，用于批量烘焙系统 logo 和背景")
    ap.add_argument("--bg", help="整屏底图。不给则用内建渐变合成静态层")
    ap.add_argument("--name", default="Default", help="皮肤显示名")
    ap.add_argument("--font", default="", help="HD 用的 TrueType 字体路径（相对 SD 卡根目录）")
    ap.add_argument("--only", choices=sorted(PROFILES), help="只烘焙这一个分辨率档位")
    ap.add_argument("--showcase", action="store_true",
                    help="系统层用整屏美术的 showcase 版式（需要 sysbg/ 资源）")
    ap.add_argument("--no-bg", action="store_true", help="不生成 bg.png，运行时用程序内合成的静态层")
    args = ap.parse_args()

    profiles = [args.only] if args.only else sorted(PROFILES)
    need_images = bool(args.theme) or (not args.no_bg)
    if need_images and not need_pillow():
        return 1

    os.makedirs(args.out, exist_ok=True)
    write_skin_ini(os.path.join(args.out, "skin.ini"), args.name, args.font)

    for prof in profiles:
        L = compute_layout(prof)
        sub = os.path.join(args.out, prof)
        os.makedirs(sub, exist_ok=True)
        print("%s (%dx%d, PAR %d:%d, 列表 %d 行)"
              % (prof, L["w"], L["h"], L["par"][0], L["par"][1], L["list_rows"]))

        write_layout_ini(os.path.join(sub, "layout.ini"), L, args.showcase)
        print("  layout.ini")

        if not args.no_bg:
            if compose_bg(os.path.join(sub, "bg.png"), L, args.bg):
                print("  bg.png %dx%d" % (L["w"], L["h"]))

        if args.theme:
            bake_systems(args.theme, sub, L)

    print("完成：%s" % args.out)
    print("把整个目录拷到 SD 卡的 skins/ 下，并在 MiSTer.ini 里设置 FBUI_SKIN=%s"
          % os.path.basename(os.path.normpath(args.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
