import json
from pathlib import Path

p = json.loads(Path(__file__).with_name("rename_plan.json").read_text(encoding="utf-8"))
out = []
for sys, rows in p.items():
    ren = sum(1 for r in rows if r["action"] == "RENAME")
    keep = sum(1 for r in rows if r["action"] == "KEEP")
    sc = sum(1 for r in rows if r["action"] == "SKIP_CN")
    sd = sum(1 for r in rows if r["action"] == "SKIP_DAT")
    out.append(f"=== {sys}: RENAME {ren} | KEEP {keep} | SKIP_CN {sc} | SKIP_DAT {sd}")
    for r in rows:
        if r["action"] in ("RENAME", "KEEP", "SKIP_CN"):
            out.append(
                f"  {r['action']}: {r['src']} -> {r.get('dst', '')} | {r.get('display', '')} | {r.get('english', '')}"
            )
Path(__file__).with_name("rename_plan.txt").write_text("\n".join(out) + "\n", encoding="utf-8")
print("\n".join(out))
