# FBUI Showcase Perf + Arcade MRA

Date: 2026-07-23

## Performance (system carousel)

**Problem:** Each left/right change synchronously decodes full-bleed JPG/PNG and SVG logo at screen size.

**Design:**
1. On selection change: immediately paint placeholder (theme bg color + system name + counter); use RAM cache blit if hit.
2. After settle debounce: decode current system into RAM cache at max ~960×540 (bg) / ~800×400 (logo), then paint.
3. Prefetch neighbors (sel±1) on subsequent idle frames.
4. LRU cache of 5 systems. Do not preload entire library.

## Arcade / CPS-1 style

**Problem:** Arcade games are `_Arcade/*.mra` (one game → core via MRA), not `games/<sys>/` + single RBF.

**Design:**
1. Inject synthetic host `Arcade` when `_Arcade` has `.mra` and `_Arcade/cores/*.rbf` exists.
2. Game list = top-level `_Arcade/*.mra` (skip `_` dirs, `cores`).
3. Launch with `xml_load(full.mra)` (same as OSD), not MGL.
4. Display name = filename without `.mra` (gamelist optional later).
