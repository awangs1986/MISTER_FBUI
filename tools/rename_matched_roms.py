#!/usr/bin/env python3
"""
Rename MiSTer ROMs to No-Intro/Redump English names when:
  1) CRC32 (preferred) or exact English filename matches a DAT entry, AND
  2) rom-name-cn has a non-empty Chinese title for that English name.

Otherwise leave the file untouched.

Display name written to gamelist.xml: 中文名(区域)  e.g. 魂斗罗(日版)

Usage (from tools/):
  python rename_matched_roms.py --dry-run
  python rename_matched_roms.py --apply
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from xml.etree import ElementTree as ET

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
CN_DIR = DATA / "rom-name-cn"
DAT_NOINTRO = DATA / "libretro-database" / "metadat" / "no-intro"
DAT_REDUMP = DATA / "libretro-database" / "metadat" / "redump"

# MiSTer games/<dir> → (DAT path, rom-name-cn CSV, extensions)
SYSTEMS = {
    "NES": (
        DAT_NOINTRO / "Nintendo - Nintendo Entertainment System.dat",
        CN_DIR / "Nintendo - Nintendo Entertainment System.csv",
        (".nes", ".fds"),
    ),
    "SNES": (
        DAT_NOINTRO / "Nintendo - Super Nintendo Entertainment System.dat",
        CN_DIR / "Nintendo - Super Nintendo Entertainment System.csv",
        (".sfc", ".smc"),
    ),
    "MegaDrive": (
        DAT_NOINTRO / "Sega - Mega Drive - Genesis.dat",
        CN_DIR / "Sega - Mega Drive - Genesis.csv",
        (".md", ".gen", ".bin", ".smd"),
    ),
    "N64": (
        DAT_NOINTRO / "Nintendo - Nintendo 64.dat",
        CN_DIR / "Nintendo - Nintendo 64.csv",
        (".n64", ".z64", ".v64"),
    ),
}

REGION_CN = {
    "USA": "美版",
    "Japan": "日版",
    "Europe": "欧版",
    "World": "世界版",
    "Asia": "亚洲版",
    "Korea": "韩版",
    "Brazil": "巴西版",
    "Australia": "澳版",
    "France": "法版",
    "Germany": "德版",
    "Spain": "西版",
    "Italy": "意版",
    "Sweden": "瑞典版",
    "Netherlands": "荷版",
    "China": "中国版",
    "Taiwan": "台湾版",
    "Hong Kong": "港版",
    "Russia": "俄版",
    "Canada": "加拿大版",
    "Mexico": "墨西哥版",
    "UK": "英版",
    "Spain, Portugal": "西葡版",
    "Japan, USA": "日美版",
    "Europe, USA": "欧美版",
    "USA, Europe": "美欧版",
    "Japan, Europe": "日欧版",
    "USA, Japan": "美日版",
    "Europe, Japan": "欧日版",
    "Japan, USA, Europe": "世界版",
    "USA, Europe, Japan": "世界版",
}

PAREN_RE = re.compile(r"\(([^()]*)\)")
ROM_BLOCK_RE = re.compile(
    r"rom\s*\(\s*"
    r'name\s+"(?P<name>(?:\\.|[^"\\])*)"\s*'
    r"(?:size\s+(?P<size>\d+)\s+)?"
    r"(?:crc\s+(?P<crc>[0-9A-Fa-f]+)\s+)?"
    r"(?:md5\s+(?P<md5>[0-9A-Fa-f]+)\s+)?"
    r"(?:sha1\s+(?P<sha1>[0-9A-Fa-f]+)\s+)?"
    r"[^)]*\)",
    re.IGNORECASE,
)
GAME_NAME_RE = re.compile(r'game\s*\(\s*name\s+"((?:\\.|[^"\\])*)"', re.IGNORECASE)


def unescape(s: str) -> str:
    return s.replace('\\"', '"').replace("\\\\", "\\")


def load_cn_map(csv_path: Path) -> dict[str, str]:
    """English No-Intro name (no extension) → Chinese title (may be empty)."""
    out: dict[str, str] = {}
    if not csv_path.is_file():
        return out
    with csv_path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        # tolerate Name EN / Name CN with spaces
        for row in reader:
            keys = {k.strip().lower(): k for k in row.keys() if k}
            en_k = keys.get("name en") or keys.get("name_en") or list(row.keys())[0]
            cn_k = keys.get("name cn") or keys.get("name_cn") or list(row.keys())[1]
            en = (row.get(en_k) or "").strip().strip('"')
            cn = (row.get(cn_k) or "").strip().strip('"')
            if not en:
                continue
            out[en] = cn
            # also index basename without needing exact punctuation variants
            out.setdefault(en.lower(), cn)
    return out


def parse_dat(dat_path: Path) -> tuple[dict[int, tuple[str, str]], dict[str, tuple[str, str]]]:
    """
    Returns:
      by_crc: crc32_int → (rom_filename, game_name)
      by_file: rom_filename.lower() → (rom_filename, game_name)
    Prefer first CRC hit; later duplicates ignored.
    """
    by_crc: dict[int, tuple[str, str]] = {}
    by_file: dict[str, tuple[str, str]] = {}
    text = dat_path.read_text(encoding="utf-8", errors="replace")
    parts = re.split(r"(?=game\s*\()", text, flags=re.IGNORECASE)
    for block in parts:
        if not re.match(r"game\s*\(", block, re.IGNORECASE):
            continue
        mname = GAME_NAME_RE.search(block)
        if not mname:
            continue
        game_name = unescape(mname.group(1))
        for m in ROM_BLOCK_RE.finditer(block):
            rom = unescape(m.group("name"))
            crc_s = m.group("crc")
            by_file.setdefault(rom.lower(), (rom, game_name))
            # also basename match
            by_file.setdefault(Path(rom).name.lower(), (rom, game_name))
            if crc_s:
                crc = int(crc_s, 16)
                by_crc.setdefault(crc, (rom, game_name))
    return by_crc, by_file


def extract_region(english_name: str) -> str:
    """Pull region tag from No-Intro name → Chinese short region."""
    parts = PAREN_RE.findall(english_name)
    if not parts:
        return ""
    # Prefer the first paren that looks like a region (not Rev, Beta, Proto, Unl, etc.)
    skip_kw = (
        "rev", "beta", "proto", "sample", "demo", "aftermarket", "unl", "pirate",
        "virtual console", "switch online", "wii", "gamecube", "en", "ja", "fr",
        "de", "es", "it", "nl", "pt", "sv", "zh", "chs", "cht", "t-en", "t-cn",
        "v1.", "v0.", "v2.", "alt", "fixed", "hack",
    )
    for p in parts:
        pl = p.lower().strip()
        if any(pl.startswith(k) or k in pl.split(",")[0].strip().lower() for k in ("rev ", "rev.", "beta", "proto", "sample", "demo")):
            continue
        if pl in ("en", "ja", "fr", "de", "es", "it", "nl", "pt", "sv", "zh"):
            continue
        if "aftermarket" in pl or "unl" == pl or pl.startswith("unl"):
            continue
        # translate known multi-region strings first
        if p in REGION_CN:
            return REGION_CN[p]
        # split "Japan, USA" style
        tokens = [t.strip() for t in p.split(",")]
        if all(t in REGION_CN or t in ("En", "Ja", "Fr", "De", "Es", "It") for t in tokens if t):
            regions = [t for t in tokens if t in REGION_CN]
            if not regions:
                continue
            if len(regions) == 1:
                return REGION_CN[regions[0]]
            key = ", ".join(regions)
            if key in REGION_CN:
                return REGION_CN[key]
            return "".join(REGION_CN[r].replace("版", "") for r in regions) + "版"
        # single known region word at start
        first = tokens[0]
        if first in REGION_CN:
            return REGION_CN[first]
    # fallback: first paren if it contains a known region word
    for p in parts:
        for eng, cn in REGION_CN.items():
            if eng in p and "," not in eng:
                return cn
    return ""


def display_name(cn: str, english_name: str) -> str:
    region = extract_region(english_name)
    if region:
        return f"{cn}({region})"
    return cn


def crc32_file(path: Path) -> list[tuple[str, int]]:
    """Return candidate (label, crc) pairs for matching."""
    data = path.read_bytes()
    out = [("full", zlib.crc32(data) & 0xFFFFFFFF)]
    # NES iNES header: also try headerless
    if path.suffix.lower() == ".nes" and len(data) > 16 and data[:4] == b"NES\x1a":
        out.append(("nohdr", zlib.crc32(data[16:]) & 0xFFFFFFFF))
    # N64 byte-swapped variants are uncommon on MiSTer (.z64 big endian) — skip
    return out


def lookup_cn(cn_map: dict[str, str], game_name: str, rom_file: str) -> str | None:
    """Return Chinese title or None if missing/empty (caller must skip rename)."""
    stem = Path(rom_file).stem
    for key in (game_name, stem, game_name.lower(), stem.lower()):
        if key in cn_map:
            cn = cn_map[key]
            if cn and cn.strip():
                return cn.strip()
            return None  # present but empty → treat as no translation
    return None


def match_rom(
    path: Path,
    by_crc: dict[int, tuple[str, str]],
    by_file: dict[str, tuple[str, str]],
    cn_map: dict[str, str],
) -> dict | None:
    """Return match dict or None if should not rename."""
    # 1) hash
    hit = None
    how = ""
    for label, crc in crc32_file(path):
        if crc in by_crc:
            hit = by_crc[crc]
            how = f"crc:{label}:{crc:08X}"
            break
    # 2) exact filename
    if not hit:
        key = path.name.lower()
        if key in by_file:
            hit = by_file[key]
            how = "filename"
    if not hit:
        return None

    rom_file, game_name = hit
    cn = lookup_cn(cn_map, game_name, rom_file)
    if not cn:
        return {
            "skip": True,
            "reason": "无中文译名",
            "match_how": how,
            "english": Path(rom_file).stem,
            "game": game_name,
            "src": path.name,
        }

    # keep original extension if DAT extension differs but same family
    dest_name = Path(rom_file).name
    # If matched via headerless CRC to .unh, use .nes with DAT game rom .nes if available
    if dest_name.lower().endswith(".unh"):
        nes_name = Path(rom_file).with_suffix(".nes").name
        # prefer .nes name from same game if present in by_file
        alt = by_file.get(nes_name.lower())
        if alt:
            dest_name = alt[0]
        else:
            dest_name = Path(rom_file).with_suffix(path.suffix).name

    if dest_name.lower() == path.name.lower():
        # already correctly named — still OK to refresh gamelist display
        return {
            "skip": False,
            "rename": False,
            "match_how": how,
            "src": path.name,
            "dst": dest_name,
            "english": Path(dest_name).stem,
            "game": game_name,
            "cn": cn,
            "display": display_name(cn, game_name),
        }

    return {
        "skip": False,
        "rename": True,
        "match_how": how,
        "src": path.name,
        "dst": dest_name,
        "english": Path(dest_name).stem,
        "game": game_name,
        "cn": cn,
        "display": display_name(cn, game_name),
    }


def update_gamelist(sys_dir: Path, renames: list[dict], dry_run: bool) -> None:
    """Patch or create gamelist entries for renamed / display-updated games."""
    xml_path = sys_dir / "gamelist.xml"
    # map old basename / new basename → display
    by_file: dict[str, str] = {}
    for r in renames:
        if r.get("skip"):
            continue
        disp = r["display"]
        by_file[r["src"]] = disp
        by_file[r["dst"]] = disp

    if not by_file:
        return

    root = None
    if xml_path.is_file():
        try:
            tree = ET.parse(xml_path)
            root = tree.getroot()
        except ET.ParseError:
            root = None

    if root is None:
        root = ET.Element("gameList")
        tree = ET.ElementTree(root)

    # index existing games by filename
    existing: dict[str, ET.Element] = {}
    for g in list(root.findall("game")):
        p = g.findtext("path") or ""
        bn = Path(p.replace("\\", "/").split("/")[-1]).name
        if bn.startswith("./"):
            bn = bn[2:]
        existing[bn.lower()] = g

    for r in renames:
        if r.get("skip"):
            continue
        src, dst = r["src"], r["dst"]
        disp = r["display"]
        base = Path(dst).stem
        g = existing.get(src.lower()) or existing.get(dst.lower())
        if g is None:
            g = ET.SubElement(root, "game")
            existing[dst.lower()] = g
        # update path/name/image/video
        def set_text(tag: str, val: str):
            node = g.find(tag)
            if node is None:
                node = ET.SubElement(g, tag)
            node.text = val

        set_text("path", f"./{dst}")
        set_text("name", disp)
        set_text("image", f"./images/{base}.png")
        set_text("video", f"./videos/{base}.mp4")

        # if renamed, drop old key
        if src.lower() != dst.lower() and src.lower() in existing:
            existing.pop(src.lower(), None)
            existing[dst.lower()] = g

    if dry_run:
        return

    # write with simple formatting
    rough = ET.tostring(root, encoding="utf-8")
    # backup
    if xml_path.is_file():
        shutil.copy2(xml_path, str(xml_path) + ".bak")
    xml_path.write_bytes(
        b'<?xml version="1.0" encoding="UTF-8"?>\n'
        b"<!-- Updated by rename_matched_roms.py: English path + CN(region) name -->\n"
        + rough
        + b"\n"
    )


def rename_sidecar(sys_dir: Path, src: str, dst: str, dry_run: bool) -> None:
    stem_s, stem_d = Path(src).stem, Path(dst).stem
    if stem_s == stem_d:
        return
    for folder, exts in (("images", (".png", ".jpg", ".jpeg", ".webp")),
                         ("videos", (".mp4", ".mkv", ".avi", ".webm"))):
        d = sys_dir / folder
        if not d.is_dir():
            continue
        for ext in exts:
            a = d / f"{stem_s}{ext}"
            b = d / f"{stem_d}{ext}"
            if a.is_file() and not b.exists():
                print(f"    media: {folder}/{a.name} -> {b.name}")
                if not dry_run:
                    a.rename(b)


def scan_system(sys_dir: Path, dat: Path, csv_path: Path, exts: tuple[str, ...]) -> list[dict]:
    by_crc, by_file = parse_dat(dat)
    cn_map = load_cn_map(csv_path)
    print(f"  DAT entries: crc={len(by_crc)} file={len(by_file)}  CN={sum(1 for v in cn_map.values() if v)}")

    results = []
    for p in sorted(sys_dir.iterdir()):
        if not p.is_file():
            continue
        if p.suffix.lower() not in exts:
            continue
        if p.name.lower() in ("gamelist.xml", "boot.rom", "boot1.rom", "boot2.rom"):
            continue
        m = match_rom(p, by_crc, by_file, cn_map)
        if m is None:
            results.append({"skip": True, "reason": "未匹配DAT", "src": p.name})
        else:
            results.append(m)
    return results


def ssh_mirror_pull(remote_sys: str, local_dir: Path) -> None:
    """Optional: not used when --local-root points at already synced folder."""
    raise NotImplementedError


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--games-root", type=Path, required=True,
                    help="Local path to games/ (e.g. mirrored from MiSTer)")
    ap.add_argument("--dry-run", action="store_true", default=False)
    ap.add_argument("--apply", action="store_true", default=False)
    ap.add_argument("--systems", default="NES,SNES,MegaDrive,N64")
    args = ap.parse_args()
    if not args.apply and not args.dry_run:
        args.dry_run = True
    if args.apply and args.dry_run:
        print("Use either --dry-run or --apply", file=sys.stderr)
        return 2

    dry = not args.apply
    print(f"Mode: {'DRY-RUN' if dry else 'APPLY'}  root={args.games_root}")

    report_lines = []
    for sys_name in [s.strip() for s in args.systems.split(",") if s.strip()]:
        if sys_name not in SYSTEMS:
            print(f"Unknown system {sys_name}, skip")
            continue
        dat, csv_path, exts = SYSTEMS[sys_name]
        sys_dir = args.games_root / sys_name
        if not sys_dir.is_dir():
            print(f"\n=== {sys_name}: missing dir {sys_dir}")
            continue
        if not dat.is_file():
            print(f"\n=== {sys_name}: missing DAT {dat}")
            continue
        print(f"\n=== {sys_name} ===")
        results = scan_system(sys_dir, dat, csv_path, exts)

        will = [r for r in results if not r.get("skip") and r.get("rename")]
        refresh = [r for r in results if not r.get("skip") and not r.get("rename")]
        skip_cn = [r for r in results if r.get("skip") and r.get("reason") == "无中文译名"]
        skip_dat = [r for r in results if r.get("skip") and r.get("reason") == "未匹配DAT"]

        print(f"  将改名: {len(will)}  已是标准名仅写中文: {len(refresh)}  "
              f"无中文跳过: {len(skip_cn)}  未匹配跳过: {len(skip_dat)}")

        for r in will:
            print(f"  RENAME [{r['match_how']}] {r['src']}")
            print(f"       -> {r['dst']}")
            print(f"       => {r['display']}")
            report_lines.append(f"{sys_name}\tRENAME\t{r['src']}\t{r['dst']}\t{r['display']}\t{r['match_how']}")
        for r in refresh:
            print(f"  KEEP   [{r['match_how']}] {r['src']} => {r['display']}")
            report_lines.append(f"{sys_name}\tKEEP\t{r['src']}\t{r['dst']}\t{r['display']}\t{r['match_how']}")
        for r in skip_cn:
            print(f"  SKIP   [无中文] {r['src']} (matched {r.get('english','')})")
            report_lines.append(f"{sys_name}\tSKIP_CN\t{r['src']}\t\t\t{r.get('match_how','')}")
        for r in skip_dat:
            print(f"  SKIP   [未匹配] {r['src']}")
            report_lines.append(f"{sys_name}\tSKIP_DAT\t{r['src']}\t\t\t")

        actionable = [r for r in results if not r.get("skip")]
        if not dry:
            for r in will:
                src_p = sys_dir / r["src"]
                dst_p = sys_dir / r["dst"]
                if dst_p.exists() and src_p.resolve() != dst_p.resolve():
                    print(f"  ! conflict, skip rename: {r['dst']} exists")
                    continue
                src_p.rename(dst_p)
                rename_sidecar(sys_dir, r["src"], r["dst"], dry_run=False)
            update_gamelist(sys_dir, actionable, dry_run=False)
            print(f"  gamelist.xml updated")
        else:
            # still show what gamelist would do
            pass

    rep = ROOT / "rename_report.tsv"
    rep.write_text("system\taction\tsrc\tdst\tdisplay\thow\n" + "\n".join(report_lines) + "\n", encoding="utf-8")
    print(f"\nReport: {rep}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
