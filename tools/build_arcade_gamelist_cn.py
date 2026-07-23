#!/usr/bin/env python3
"""Build /media/fat/_Arcade/gamelist.xml with Chinese names.

Sources (priority):
  1) rom-name-cn Arcade-*.csv  (CPS1/2/3 + NeoGeo)
  2) mame_cn.lst               (broad MAME setname list)
  3) parent setname fallback   (strip region/version suffixes)

Match order per MRA:
  a) <setname> exact → csv / lst
  b) <name> / filename stem → EN map
  c) parent/fuzzy setname → lst / csv
"""
from __future__ import annotations

import csv
import re
from pathlib import Path
from xml.sax.saxutils import escape

ARCADE = Path("/media/fat/_Arcade")
CN_DIR = Path("/tmp/rom-name-cn")
MAME_LST = Path("/tmp/rom-name-cn/mame_cn.lst")

SETNAME_RE = re.compile(r"<setname>\s*([^<]+?)\s*</setname>", re.I)
NAME_RE = re.compile(r"<name>\s*([^<]+?)\s*</name>", re.I)
SUFFIXES = [
    "jm72", "m72", "m92", "m82", "m81",
    "bl", "jt", "ua", "ja", "ub", "jb", "ab", "oj", "ou", "ea",
    "j", "u", "a", "b", "o", "e", "k", "w", "h", "n", "p", "r", "s", "t",
]


def has_cjk(s: str) -> bool:
    return any("\u4e00" <= c <= "\u9fff" for c in s)


def load_cn_maps(cn_dir: Path) -> tuple[dict[str, str], dict[str, str]]:
    by_set: dict[str, str] = {}
    by_en: dict[str, str] = {}
    for csv_path in sorted(cn_dir.glob("*.csv")):
        with csv_path.open("r", encoding="utf-8-sig", newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                keys = {(k or "").strip().lower(): k for k in row}
                mame_k = keys.get("mame name") or keys.get("mame") or list(row.keys())[0]
                en_k = keys.get("en name") or keys.get("name en")
                cn_k = keys.get("cn name") or keys.get("name cn")
                mame = (row.get(mame_k) or "").strip().strip('"')
                en = (row.get(en_k) or "").strip().strip('"') if en_k else ""
                cn = (row.get(cn_k) or "").strip().strip('"') if cn_k else ""
                if not cn or not has_cjk(cn):
                    continue
                if mame:
                    by_set.setdefault(mame.lower(), cn)
                if en:
                    by_en.setdefault(en.lower(), cn)
    return by_set, by_en


def load_mame_lst(path: Path) -> dict[str, str]:
    by_set: dict[str, str] = {}
    if not path.is_file():
        return by_set
    for ln in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        parts = ln.split("\t")
        if len(parts) < 2:
            continue
        setname, cn = parts[0].strip(), parts[1].strip()
        if setname and cn and has_cjk(cn):
            by_set.setdefault(setname.lower(), cn)
    return by_set


def parent_lookup(setname: str, table: dict[str, str]) -> str:
    s = setname.lower()
    if not s:
        return ""
    if s in table:
        return table[s]
    cur = s
    for _ in range(6):
        changed = False
        for suf in SUFFIXES:
            if len(cur) > len(suf) + 2 and cur.endswith(suf):
                cur2 = cur[: -len(suf)]
                if cur2 in table:
                    return table[cur2]
                cur = cur2
                changed = True
                break
        m = re.search(r"\d+$", cur)
        if m and len(cur) - len(m.group(0)) >= 3:
            cur2 = cur[: m.start()]
            if cur2 in table:
                return table[cur2]
            cur = cur2
            changed = True
        if not changed:
            break
    best = ""
    for k in table:
        if len(k) >= 4 and s.startswith(k) and len(k) > len(best):
            best = k
    return table.get(best, "")


def parse_mra(path: Path) -> tuple[str, str]:
    data = path.read_text(encoding="utf-8", errors="replace")[:16384]
    sm = SETNAME_RE.search(data)
    nm = NAME_RE.search(data)
    setname = sm.group(1).strip() if sm else ""
    name = nm.group(1).strip() if nm else path.stem
    return setname, name


def main() -> int:
    if not ARCADE.is_dir():
        print("no _Arcade")
        return 1
    if not CN_DIR.is_dir():
        print(f"missing {CN_DIR}")
        return 1

    by_set, by_en = load_cn_maps(CN_DIR)
    lst = load_mame_lst(MAME_LST)
    print(f"rom-name-cn: setname={len(by_set)} en={len(by_en)}")
    print(f"mame_cn.lst: setname={len(lst)}")

    mras = sorted(
        p for p in ARCADE.iterdir()
        if p.is_file() and p.suffix.lower() == ".mra" and not p.name.startswith("_")
    )

    hit_csv = hit_lst = hit_fuzzy = miss = 0
    miss_samples: list[str] = []
    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        "<!-- Arcade Chinese: rom-name-cn > mame_cn.lst > parent setname -->",
        "<gameList>",
    ]
    for p in mras:
        setname, en_name = parse_mra(p)
        cn = ""
        src = "miss"
        if setname and setname.lower() in by_set:
            cn = by_set[setname.lower()]
            src = "csv"
        if not cn:
            cn = by_en.get(en_name.lower(), "") or by_en.get(p.stem.lower(), "")
            if cn:
                src = "csv"
        if not cn and setname and setname.lower() in lst:
            cn = lst[setname.lower()]
            src = "lst"
        if not cn and setname:
            cn = parent_lookup(setname, lst) or parent_lookup(setname, by_set)
            if cn:
                src = "fuzzy"
        if cn:
            if src == "csv":
                hit_csv += 1
            elif src == "lst":
                hit_lst += 1
            else:
                hit_fuzzy += 1
            disp = cn
        else:
            miss += 1
            disp = en_name or p.stem
            if len(miss_samples) < 40:
                miss_samples.append(f"{p.name}|set={setname}")
        lines += [
            "  <game>",
            f"    <path>./{escape(p.name)}</path>",
            f"    <name>{escape(disp)}</name>",
            "  </game>",
        ]

    xml = ARCADE / "gamelist.xml"
    if xml.is_file():
        xml.rename(str(xml) + ".bak")
    xml.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {len(mras)} hit_csv={hit_csv} hit_lst={hit_lst} hit_fuzzy={hit_fuzzy} miss={miss} -> {xml}")
    for s in miss_samples:
        print("MISS", s)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
