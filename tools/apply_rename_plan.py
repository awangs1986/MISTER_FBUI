#!/usr/bin/env python3
"""Apply rename_plan.json on MiSTer games/ directories."""
from __future__ import annotations

import json
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path

GAMES = Path("/media/fat/games")
PLAN = Path("/tmp/rename_plan.json")

# Prefer the Chinese-named source when two files share the same destination.
PREFER_SRC_FOR_DST = {
    "Huoqiang Yingxiong (China) (Pirate).md": "huo qiang ying xiong.bin",
}


def load_plan():
    return json.loads(PLAN.read_text(encoding="utf-8"))


def ensure_gamelist(sys_dir: Path) -> ET.Element:
    xml_path = sys_dir / "gamelist.xml"
    if xml_path.is_file():
        try:
            tree = ET.parse(xml_path)
            return tree.getroot()
        except ET.ParseError:
            pass
    return ET.Element("gameList")


def index_games(root: ET.Element) -> dict[str, ET.Element]:
    out = {}
    for g in root.findall("game"):
        p = (g.findtext("path") or "").replace("\\", "/")
        bn = Path(p.split("/")[-1]).name
        if bn.startswith("./"):
            bn = bn[2:]
        out[bn.lower()] = g
    return out


def set_text(g: ET.Element, tag: str, val: str):
    node = g.find(tag)
    if node is None:
        node = ET.SubElement(g, tag)
    node.text = val


def rename_sidecars(sys_dir: Path, src: str, dst: str):
    stem_s, stem_d = Path(src).stem, Path(dst).stem
    if stem_s == stem_d:
        return
    for folder, exts in (
        ("images", (".png", ".jpg", ".jpeg", ".webp")),
        ("videos", (".mp4", ".mkv", ".avi", ".webm")),
    ):
        d = sys_dir / folder
        if not d.is_dir():
            continue
        for ext in exts:
            a, b = d / f"{stem_s}{ext}", d / f"{stem_d}{ext}"
            if a.is_file() and not b.exists():
                print(f"  media {a.name} -> {b.name}")
                a.rename(b)


def write_gamelist(sys_dir: Path, root: ET.Element):
    xml_path = sys_dir / "gamelist.xml"
    if xml_path.is_file():
        shutil.copy2(xml_path, str(xml_path) + ".bak_rename")
    # Pretty-ish write
    try:
        ET.indent(root, space="  ")
    except AttributeError:
        pass
    body = ET.tostring(root, encoding="utf-8")
    xml_path.write_bytes(
        b'<?xml version="1.0" encoding="UTF-8"?>\n'
        b"<!-- Updated by apply_rename_plan.py -->\n" + body + b"\n"
    )


def main():
    plan = load_plan()
    for sys_name, rows in plan.items():
        sys_dir = GAMES / sys_name
        if not sys_dir.is_dir():
            print(f"=== {sys_name}: missing, skip")
            continue
        print(f"=== {sys_name} ===")

        # Resolve destination conflicts: same dst from multiple RENAMEs
        renames = [r for r in rows if r["action"] == "RENAME"]
        keeps = [r for r in rows if r["action"] == "KEEP"]

        by_dst: dict[str, list[dict]] = {}
        for r in renames:
            by_dst.setdefault(r["dst"].lower(), []).append(r)

        chosen_renames = []
        for dst_l, group in by_dst.items():
            if len(group) == 1:
                chosen_renames.append(group[0])
                continue
            prefer = PREFER_SRC_FOR_DST.get(group[0]["dst"])
            pick = None
            if prefer:
                for g in group:
                    if g["src"] == prefer:
                        pick = g
                        break
            if pick is None:
                pick = group[0]
            print(f"  DUP dst {pick['dst']}: keep {pick['src']}, remove others")
            for g in group:
                if g is pick:
                    chosen_renames.append(g)
                else:
                    src_p = sys_dir / g["src"]
                    if src_p.is_file():
                        bak = sys_dir / (g["src"] + ".dup_removed")
                        # just delete duplicate content
                        print(f"  REMOVE dup {g['src']}")
                        src_p.unlink()

        root = ensure_gamelist(sys_dir)
        existing = index_games(root)

        for r in chosen_renames:
            src_p = sys_dir / r["src"]
            dst_p = sys_dir / r["dst"]
            if not src_p.is_file():
                print(f"  ! missing {r['src']}")
                continue
            if dst_p.exists() and src_p.resolve() != dst_p.resolve():
                print(f"  ! conflict exists {r['dst']}, skip")
                continue
            print(f"  RENAME {r['src']} -> {r['dst']}")
            src_p.rename(dst_p)
            rename_sidecars(sys_dir, r["src"], r["dst"])
            # gamelist
            g = existing.pop(r["src"].lower(), None) or existing.get(r["dst"].lower())
            if g is None:
                g = ET.SubElement(root, "game")
            set_text(g, "path", f"./{r['dst']}")
            set_text(g, "name", r["display"])
            base = Path(r["dst"]).stem
            set_text(g, "image", f"./images/{base}.png")
            set_text(g, "video", f"./videos/{base}.mp4")
            existing[r["dst"].lower()] = g

        for r in keeps:
            # only update display name in gamelist; file stays
            dst = r.get("dst") or r["src"]
            g = existing.get(r["src"].lower()) or existing.get(dst.lower())
            if g is None:
                g = ET.SubElement(root, "game")
                existing[dst.lower()] = g
            set_text(g, "path", f"./{dst}")
            set_text(g, "name", r["display"])
            base = Path(dst).stem
            set_text(g, "image", f"./images/{base}.png")
            set_text(g, "video", f"./videos/{base}.mp4")
            print(f"  KEEP/NAME {dst} => {r['display']}")

        write_gamelist(sys_dir, root)
        print(f"  gamelist.xml updated")

    print("DONE")


if __name__ == "__main__":
    main()
