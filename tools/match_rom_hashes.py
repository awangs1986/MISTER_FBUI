#!/usr/bin/env python3
"""Match MiSTer ROM hashes against DAT + rom-name-cn; optionally rename on device via report.

Rules:
  - CRC preferred, then exact English filename
  - No DAT match → skip
  - No Chinese translation → skip (do not rename)
  - Display: 中文名(区域)
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
CN_DIR = DATA / "rom-name-cn"
DAT_NOINTRO = DATA / "libretro-database" / "metadat" / "no-intro"

SYSTEMS = {
    "NES": (
        DAT_NOINTRO / "Nintendo - Nintendo Entertainment System.dat",
        CN_DIR / "Nintendo - Nintendo Entertainment System.csv",
    ),
    "SNES": (
        DAT_NOINTRO / "Nintendo - Super Nintendo Entertainment System.dat",
        CN_DIR / "Nintendo - Super Nintendo Entertainment System.csv",
    ),
    "MegaDrive": (
        DAT_NOINTRO / "Sega - Mega Drive - Genesis.dat",
        CN_DIR / "Sega - Mega Drive - Genesis.csv",
    ),
    "N64": (
        DAT_NOINTRO / "Nintendo - Nintendo 64.dat",
        CN_DIR / "Nintendo - Nintendo 64.csv",
    ),
}

REGION_CN = {
    "USA": "美版", "Japan": "日版", "Europe": "欧版", "World": "世界版",
    "Asia": "亚洲版", "Korea": "韩版", "Brazil": "巴西版", "Australia": "澳版",
    "France": "法版", "Germany": "德版", "Spain": "西版", "Italy": "意版",
    "Sweden": "瑞典版", "Netherlands": "荷版", "China": "中国版",
    "Taiwan": "台湾版", "Hong Kong": "港版", "Russia": "俄版", "Canada": "加拿大版",
    "Japan, USA": "日美版", "Europe, USA": "欧美版", "USA, Europe": "美欧版",
    "Japan, Europe": "日欧版", "USA, Japan": "美日版", "Europe, Japan": "欧日版",
    "Japan, USA, Europe": "世界版", "USA, Europe, Japan": "世界版",
}

PAREN_RE = re.compile(r"\(([^()]*)\)")
ROM_BLOCK_RE = re.compile(
    r"rom\s*\(\s*"
    r'name\s+"(?P<name>(?:\\.|[^"\\])*)"\s*'
    r"(?:size\s+(?P<size>\d+)\s+)?"
    r"(?:crc\s+(?P<crc>[0-9A-Fa-f]+)\s+)?"
    r"[^)]*\)",
    re.IGNORECASE,
)
GAME_NAME_RE = re.compile(r'game\s*\(\s*name\s+"((?:\\.|[^"\\])*)"', re.IGNORECASE)


def unescape(s: str) -> str:
    return s.replace('\\"', '"').replace("\\\\", "\\")


def load_cn_map(csv_path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    with csv_path.open("r", encoding="utf-8-sig", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            keys = {((k or "").strip().lower()): k for k in row.keys()}
            en_k = keys.get("name en") or list(row.keys())[0]
            cn_k = keys.get("name cn") or list(row.keys())[1]
            en = (row.get(en_k) or "").strip().strip('"')
            cn = (row.get(cn_k) or "").strip().strip('"')
            if en:
                out[en] = cn
                out[en.lower()] = cn
    return out


def parse_dat(dat_path: Path):
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
            by_file.setdefault(rom.lower(), (rom, game_name))
            by_file.setdefault(Path(rom).name.lower(), (rom, game_name))
            crc_s = m.group("crc")
            if crc_s:
                by_crc.setdefault(int(crc_s, 16), (rom, game_name))
    return by_crc, by_file


def extract_region(english_name: str) -> str:
    parts = PAREN_RE.findall(english_name)
    for p in parts:
        pl = p.lower().strip()
        if any(pl.startswith(k) for k in ("rev", "beta", "proto", "sample", "demo", "v0.", "v1.", "v2.")):
            continue
        if pl in ("en", "ja", "fr", "de", "es", "it", "nl", "pt", "sv", "zh"):
            continue
        if "aftermarket" in pl or pl in ("unl", "pirate"):
            continue
        if p in REGION_CN:
            return REGION_CN[p]
        tokens = [t.strip() for t in p.split(",")]
        regions = [t for t in tokens if t in REGION_CN]
        if regions:
            if len(regions) == 1:
                return REGION_CN[regions[0]]
            key = ", ".join(regions)
            if key in REGION_CN:
                return REGION_CN[key]
            return "".join(REGION_CN[r].replace("版", "") for r in regions) + "版"
        first = tokens[0]
        if first in REGION_CN:
            return REGION_CN[first]
    for p in parts:
        for eng, cn in REGION_CN.items():
            if eng in p and "," not in eng:
                return cn
    return ""


def display_name(cn: str, english_name: str) -> str:
    region = extract_region(english_name)
    return f"{cn}({region})" if region else cn


def lookup_cn(cn_map: dict[str, str], game_name: str, rom_file: str) -> str | None:
    stem = Path(rom_file).stem
    for key in (game_name, stem, game_name.lower(), stem.lower()):
        if key in cn_map:
            cn = (cn_map[key] or "").strip()
            return cn if cn else None
    return None


def prefer_rom_name(rom_file: str, src_name: str, by_file: dict) -> str:
    """Prefer cartridge extension matching source; avoid .unh."""
    if rom_file.lower().endswith(".unh"):
        nes = str(Path(rom_file).with_suffix(".nes"))
        if nes.lower() in by_file:
            return by_file[nes.lower()][0]
        return str(Path(rom_file).with_suffix(Path(src_name).suffix))
    # if DAT has same stem with source extension, prefer that
    stem = Path(rom_file).stem
    cand = f"{stem}{Path(src_name).suffix}"
    if cand.lower() in by_file:
        return by_file[cand.lower()][0]
    return Path(rom_file).name


def match_one(rec: dict, by_crc, by_file, cn_map) -> dict:
    src = rec["name"]
    hit = None
    how = ""
    for key in ("crc", "crc_nohdr"):
        if key not in rec:
            continue
        crc = int(rec[key], 16)
        if crc in by_crc:
            hit = by_crc[crc]
            how = f"{key}:{rec[key]}"
            break
    if not hit:
        k = src.lower()
        if k in by_file:
            hit = by_file[k]
            how = "filename"
    if not hit:
        return {"action": "SKIP_DAT", "src": src}

    rom_file, game_name = hit
    cn = lookup_cn(cn_map, game_name, rom_file)
    if not cn:
        return {
            "action": "SKIP_CN",
            "src": src,
            "english": Path(rom_file).stem,
            "game": game_name,
            "how": how,
        }

    dst = prefer_rom_name(rom_file, src, by_file)
    disp = display_name(cn, game_name)
    if dst.lower() == src.lower():
        return {
            "action": "KEEP",
            "src": src,
            "dst": dst,
            "display": disp,
            "game": game_name,
            "how": how,
        }
    return {
        "action": "RENAME",
        "src": src,
        "dst": dst,
        "display": disp,
        "game": game_name,
        "how": how,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hashes", type=Path, required=True, help="JSON from MiSTer hash script")
    ap.add_argument("--out", type=Path, default=ROOT / "rename_plan.json")
    args = ap.parse_args()

    hashes = json.loads(args.hashes.read_text(encoding="utf-8"))
    # strip possible SSH noise before {
    if isinstance(hashes, str):
        hashes = json.loads(hashes[hashes.index("{") :])

    plan = {}
    for sys_name, (dat, csv_path) in SYSTEMS.items():
        recs = hashes.get(sys_name) or []
        if not dat.is_file() or not csv_path.is_file():
            print(f"=== {sys_name}: missing DAT/CSV")
            continue
        by_crc, by_file = parse_dat(dat)
        cn_map = load_cn_map(csv_path)
        print(f"\n=== {sys_name} ({len(recs)} files) DAT crc={len(by_crc)} CN={sum(1 for v in cn_map.values() if v)} ===")
        rows = [match_one(r, by_crc, by_file, cn_map) for r in recs]
        plan[sys_name] = rows
        for a in ("RENAME", "KEEP", "SKIP_CN", "SKIP_DAT"):
            n = sum(1 for r in rows if r["action"] == a)
            if n:
                print(f"  {a}: {n}")
        for r in rows:
            if r["action"] == "RENAME":
                print(f"  RENAME [{r['how']}] {r['src']}")
                print(f"       -> {r['dst']}")
                print(f"       => {r['display']}")
            elif r["action"] == "KEEP":
                print(f"  KEEP   [{r['how']}] {r['src']} => {r['display']}")
            elif r["action"] == "SKIP_CN":
                print(f"  SKIP_CN [{r.get('how')}] {r['src']} (matched {r.get('english')})")
            else:
                print(f"  SKIP_DAT {r['src']}")

    args.out.write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\nPlan written: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
