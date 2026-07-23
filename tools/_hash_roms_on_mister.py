import os, zlib, json
from pathlib import Path
ROOT = Path("/media/fat/games")
SYSTEMS = {
  "NES": {".nes", ".fds"},
  "SNES": {".sfc", ".smc"},
  "MegaDrive": {".md", ".gen", ".bin", ".smd"},
  "N64": {".n64", ".z64", ".v64"},
}
out = {}
for sys, exts in SYSTEMS.items():
  d = ROOT / sys
  items = []
  if d.is_dir():
    for p in sorted(d.iterdir()):
      if not p.is_file():
        continue
      if p.suffix.lower() not in exts:
        continue
      if p.name.lower().startswith("boot"):
        continue
      data = p.read_bytes()
      full = zlib.crc32(data) & 0xffffffff
      rec = {"name": p.name, "size": len(data), "crc": f"{full:08X}"}
      if p.suffix.lower() == ".nes" and len(data) > 16 and data[:4] == b"NES\x1a":
        rec["crc_nohdr"] = f"{(zlib.crc32(data[16:]) & 0xffffffff):08X}"
      items.append(rec)
  out[sys] = items
print(json.dumps(out, ensure_ascii=False))
