#!/usr/bin/env python3
"""Rebuild slim gamelist.xml for each system: only on-disk ROMs.
Uses rename_plan.json Chinese display names when available."""
from __future__ import annotations

import json
import re
from pathlib import Path
from xml.sax.saxutils import escape

GAMES = Path("/media/fat/games")
PLAN = Path("/tmp/rename_plan.json")

EXTS = {
    "NES": {".nes", ".fds"},
    "SNES": {".sfc", ".smc"},
    "MegaDrive": {".md", ".gen", ".bin", ".smd"},
    "N64": {".n64", ".z64", ".v64"},
}


def main():
    plan = json.loads(PLAN.read_text(encoding="utf-8"))
    disp_map = {}  # (sys, filename.lower()) -> display
    for sys, rows in plan.items():
        for r in rows:
            if r.get("action") in ("RENAME", "KEEP") and r.get("display"):
                dst = r.get("dst") or r["src"]
                disp_map[(sys, dst.lower())] = r["display"]
                disp_map[(sys, r["src"].lower())] = r["display"]

    for sys, exts in EXTS.items():
        d = GAMES / sys
        if not d.is_dir():
            continue
        files = sorted(
            p.name
            for p in d.iterdir()
            if p.is_file()
            and p.suffix.lower() in exts
            and not p.name.lower().startswith("boot")
        )
        lines = [
            '<?xml version="1.0" encoding="UTF-8"?>',
            "<!-- slim gamelist: on-disk ROMs only -->",
            "<gameList>",
        ]
        for fn in files:
            disp = disp_map.get((sys, fn.lower()), Path(fn).stem)
            base = Path(fn).stem
            lines += [
                "  <game>",
                f"    <path>./{escape(fn)}</path>",
                f"    <name>{escape(disp)}</name>",
                f"    <image>./images/{escape(base)}.png</image>",
                f"    <video>./videos/{escape(base)}.mp4</video>",
                "  </game>",
            ]
        lines.append("</gameList>")
        xml = d / "gamelist.xml"
        if xml.is_file():
            xml.rename(str(xml) + ".bak_full")
        xml.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"{sys}: wrote {len(files)} entries")


if __name__ == "__main__":
    main()
