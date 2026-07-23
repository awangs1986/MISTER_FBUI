#!/usr/bin/env python3
"""
Generate EmulationStation gamelist.xml files from No-Intro / Redump DATs
for MiSTer game folders — assuming a complete ROM set named like No-Intro.

Output layout (ready to merge onto the SD card):
  <out>/NES/gamelist.xml
  <out>/SNES/gamelist.xml
  ...

Media paths (files optional; FBUI leaves the panel empty if missing):
  ./images/<rom_basename>.png
  ./videos/<rom_basename>.mp4
"""

from __future__ import annotations

import argparse
import html
import os
import re
import sys
from pathlib import Path

# MiSTer games/<dir> -> (DAT relative path under metadat/, preferred rom extensions)
SYSTEMS = [
    ("NES",        "no-intro/Nintendo - Nintendo Entertainment System.dat", ("nes",)),
    ("SNES",       "no-intro/Nintendo - Super Nintendo Entertainment System.dat", ("sfc", "smc")),
    ("MegaDrive",  "no-intro/Sega - Mega Drive - Genesis.dat", ("md", "gen", "bin")),
    ("SMS",        "no-intro/Sega - Master System - Mark III.dat", ("sms",)),
    ("GBA",        "no-intro/Nintendo - Game Boy Advance.dat", ("gba",)),
    ("GAMEBOY",    "no-intro/Nintendo - Game Boy.dat", ("gb",)),
    # GBC shares GAMEBOY folder on many MiSTer setups; also emit a companion list
    ("GAMEBOY",    "no-intro/Nintendo - Game Boy Color.dat", ("gbc",), "append"),
    ("N64",        "no-intro/Nintendo - Nintendo 64.dat", ("n64", "z64", "v64")),
    ("TGFX16",     "no-intro/NEC - PC Engine - TurboGrafx 16.dat", ("pce",)),
    ("Atari2600",  "no-intro/Atari - 2600.dat", ("a26",)),
    ("Atari7800",  "no-intro/Atari - 7800.dat", ("a78",)),
    ("AtariLynx",  "no-intro/Atari - Lynx.dat", ("lnx",)),
    ("WonderSwan", "no-intro/Bandai - WonderSwan.dat", ("ws",)),
    ("WonderSwan", "no-intro/Bandai - WonderSwan Color.dat", ("wsc",), "append"),
    ("PSX",        "redump/Sony - PlayStation.dat", ("cue", "chd", "iso")),
    ("Saturn",     "redump/Sega - Saturn.dat", ("cue", "chd", "iso")),
    ("MegaCD",     "redump/Sega - Mega-CD - Sega CD.dat", ("cue", "chd", "iso")),
    ("TGFX16-CD",  "redump/NEC - PC Engine CD - TurboGrafx-CD.dat", ("cue", "chd", "iso")),
]

GAME_RE = re.compile(
    r'game\s*\(\s*'
    r'name\s+"(?P<name>(?:\\.|[^"\\])*)"\s*'
    r'(?:.*?rom\s*\(\s*name\s+"(?P<rom>(?:\\.|[^"\\])*)")?',
    re.DOTALL | re.IGNORECASE,
)
ROM_RE = re.compile(r'rom\s*\(\s*name\s+"((?:\\.|[^"\\])*)"', re.IGNORECASE)


def unescape(s: str) -> str:
    return s.replace('\\"', '"').replace("\\\\", "\\")


def display_name(name: str) -> str:
    """Keep No-Intro title but drop trailing dump tags for UI readability."""
    # strip only the last (...) groups that look like region/rev tags? keep full name —
    # users matching Screenscraper prefer the full No-Intro string.
    return name


def parse_dat(path: Path, exts: tuple[str, ...], *, invent_cue: bool = False) -> list[tuple[str, str]]:
    """Return list of (display_name, rom_filename) for preferred extensions."""
    text = path.read_text(encoding="utf-8", errors="replace")
    out: list[tuple[str, str]] = []
    seen = set()

    parts = re.split(r"(?=game\s*\()", text, flags=re.IGNORECASE)
    for block in parts:
        if not re.match(r"game\s*\(", block, re.IGNORECASE):
            continue
        mname = re.search(r'name\s+"((?:\\.|[^"\\])*)"', block)
        if not mname:
            continue
        gname = unescape(mname.group(1))
        roms = [unescape(r) for r in ROM_RE.findall(block)]
        chosen = None
        for ext in exts:
            for rom in roms:
                if rom.lower().endswith("." + ext.lower()):
                    chosen = rom
                    break
            if chosen:
                break
        if not chosen and invent_cue:
            # Redump DATs list .bin tracks; MiSTer/ES usually mount the .cue
            chosen = gname + ".cue"
        if not chosen:
            continue
        key = chosen.lower()
        if key in seen:
            continue
        seen.add(key)
        out.append((gname, chosen))
    return out


def xml_escape(s: str) -> str:
    return html.escape(s, quote=True)


def write_gamelist(path: Path, games: list[tuple[str, str]], append: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    existing: list[tuple[str, str]] = []
    if append and path.exists():
        # crude re-parse of our own output
        body = path.read_text(encoding="utf-8")
        for m in re.finditer(
            r"<path>\./([^<]+)</path>\s*<name>([^<]*)</name>", body
        ):
            existing.append((html.unescape(m.group(2)), m.group(1)))

    merged = existing + games
    # de-dupe by rom filename
    seen = set()
    uniq: list[tuple[str, str]] = []
    for name, rom in merged:
        k = rom.lower()
        if k in seen:
            continue
        seen.add(k)
        uniq.append((name, rom))
    uniq.sort(key=lambda t: t[1].lower())

    lines = [
        '<?xml version="1.0"?>',
        "<!-- Generated from No-Intro / Redump via tools/gen_gamelists.py -->",
        "<!-- images/ and videos/ use the ROM basename; missing files = empty preview -->",
        "<gameList>",
    ]
    for name, rom in uniq:
        base, _dot, _ext = rom.rpartition(".")
        if not base:
            base = rom
        disp = display_name(name)
        lines.append("  <game>")
        lines.append(f"    <path>./{xml_escape(rom)}</path>")
        lines.append(f"    <name>{xml_escape(disp)}</name>")
        lines.append(f"    <image>./images/{xml_escape(base)}.png</image>")
        lines.append(f"    <video>./videos/{xml_escape(base)}.mp4</video>")
        lines.append("  </game>")
    lines.append("</gameList>")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"  {path}: {len(uniq)} games")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--dat-root",
        type=Path,
        default=Path(r"D:/temp/libretro-db/metadat"),
        help="Folder containing no-intro/ and redump/",
    )
    ap.add_argument(
        "--out",
        type=Path,
        default=Path(r"D:/otherproject/misterfpga/out/games"),
        help="Output root (writes <out>/<MiSTerDir>/gamelist.xml)",
    )
    args = ap.parse_args()

    if not args.dat_root.is_dir():
        print(f"DAT root not found: {args.dat_root}", file=sys.stderr)
        return 1

    # normalize SYSTEMS entries to 4-tuples
    specs = []
    for row in SYSTEMS:
        if len(row) == 3:
            specs.append((row[0], row[1], row[2], "replace"))
        else:
            specs.append(row)

    args.out.mkdir(parents=True, exist_ok=True)
    for mister_dir, dat_rel, exts, mode in specs:
        dat_path = args.dat_root / dat_rel
        if not dat_path.is_file():
            print(f"SKIP {mister_dir}: missing {dat_rel}")
            continue
        print(f"{mister_dir} <- {dat_rel}")
        games = parse_dat(dat_path, exts, invent_cue=("cue" in exts))
        if not games:
            print(f"  (no roms matched extensions {exts})")
            continue
        out_xml = args.out / mister_dir / "gamelist.xml"
        write_gamelist(out_xml, games, append=(mode == "append"))

    print(f"\nDone. Copy folders under {args.out} into /media/fat/games/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
