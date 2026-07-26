# Code Review Guide — MISTER_FBUI

This document tells an external reviewer what to review, what to ignore, how to
build it, and which constraints any suggested change has to respect.

Read section 1–3 before opening any code. The repository contains ~2000 upstream
commits that are **not** ours and must not be reviewed.

---

## 1. TL;DR

| | |
|---|---|
| What it is | A fork of [MiSTer-devel/Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer) — the ARM/Linux userspace binary for the DE10-Nano — with a custom full-screen game browser ("FBUI") added |
| Language / target | C++ (gnu++14) and C (gnu99), cross-compiled for ARMv7 Cortex-A9, `-O3 -Wall -Wextra` |
| Review scope | The diff `2025dc7..HEAD` — 14 commits, ~3800 lines of new hand-written C++ plus ~450 lines of edits to upstream files |
| Out of scope | Everything else, including the vendored 5079-line `lib/stb/stb_truetype.h` |
| Biggest risk area | `fbui.cpp` (2511 lines) — hand-rolled software rasteriser, byte-budgeted caches, and a subprocess launcher |
| Test coverage | None. There is no test target, no CI for our code, and no way to unit-test the current structure |

---

## 2. What the project does

MiSTer runs Linux on the DE10-Nano's ARM (HPS) side while FPGA cores implement
the actual retro hardware. The stock game-selection UI is a character-cell OSD
drawn by the FPGA — fast and reliable, but limited to a fixed font, 8 colours
and no images.

FBUI replaces that selection UI with a full-screen graphical browser drawn in
software by the ARM into the HPS framebuffer plane, which the FPGA scaler then
composites to HDMI and VGA. **No HDL was changed** — this is entirely an
ARM-side userspace addition.

Two output profiles are supported and are mutually exclusive, chosen by a
picker shown at boot:

- **1080p** over HDMI, TrueType text, photographic artwork.
- **240p** over analog VGA→RGBS for CRTs, 640x240 with a 1:2 pixel aspect
  ratio, bitmap text. The non-square pixels are the source of a lot of the
  odd-looking arithmetic; see section 6.

---

## 3. Review scope — exactly what is ours

Everything we wrote sits on top of upstream commit `2025dc7`:

```bash
git log --oneline 2025dc7..HEAD      # 14 commits
git diff --stat 2025dc7..HEAD        # the full review surface
```

### 3.1 Please review, in priority order

| Priority | File | Lines | What it is |
|---|---|---|---|
| 1 | `fbui.cpp` | 2511 | Browser state, layout, input, directory scanning, software rasteriser, both cache subsystems, video preview, boot resolution picker |
| 2 | `fbui_skin.cpp` / `.h` | 677 / 123 | Skin package loading, INI layout parsing, image decode + composite |
| 3 | `fbui_text.cpp` / `.h` | 283 / 34 | TrueType text engine and glyph cache (HD profile only) |
| 4 | `video.cpp` / `.h` | +135−1 / +16 | Menu-time video mode switching (`video_menu_res_apply` / `_restore`) |
| 5 | `osd.cpp` / `.h` | +165−16 / +4 | Hardware-OSD additions used by the boot resolution picker |
| 6 | `menu.cpp` | +55−12 | The hook that hands control to FBUI |
| 7 | `file_io.cpp` / `.h` | +38−7 / +8 | Path and asset lookup helpers |
| 8 | `font_cjk.cpp` / `.h` | 98 / 22 | UTF-8 decoding and CJK helpers |
| 9 | `cfg.cpp` / `.h` | +3 / +3 | Two new INI options |

Priorities 4–7 deserve disproportionate attention relative to their size: they
are edits to **shared upstream files**, so a defect there breaks users who never
enable FBUI, and a large diff there makes future rebases onto upstream painful.

### 3.2 Lower priority — offline tooling

These never run on the target device; they run on a PC. Correctness matters,
but crashes are cheap and there is no security boundary.

- `tools/skinbake/bake.py` (504) — offline skin baker, Python + Pillow
- `tools/skinpreview/` (~850) — self-contained HTML/JS layout preview
- `tools/GamelistEditor/` (~1350) — C# WinForms game-list editor
- `tools/*.py` (~1470) — one-off ROM matching and renaming scripts

### 3.3 Explicitly out of scope — please do not report findings here

- `lib/stb/stb_truetype.h` — vendored third-party, unmodified
- Any file not listed in `git diff --stat 2025dc7..HEAD`
- `docs/chat-backup/` — raw development transcripts, not code
- Upstream MiSTer's own architecture, style, or defects

---

## 4. Building

The binary is cross-compiled. A Linux host or WSL2 is assumed; the Makefile does
not work under native Windows.

```bash
# One-time: fetch ARM GNU toolchain 10.2-2020.11 (~500 MB).
# Note the MISTER_GCC_INSTALL_DIR: the upstream script defaults to unpacking
# into the repo root, but build.sh looks in $HOME. Setting it here keeps the
# two consistent; otherwise export ARM_TOOLCHAIN_DIR to wherever it landed.
MISTER_GCC_INSTALL_DIR="$HOME" source ./setup_default_toolchain.sh

# Build.
./build.sh -j"$(nproc)"
```

`build.sh` only prepends the toolchain to `PATH`; the Makefile derives the
compiler itself via `CC = $(BASE)-gcc`. So the `CC` that
`setup_default_toolchain.sh` exports is never used — which is just as well,
since upstream spells it `arm-none-gnueabihf-gcc`, missing the `linux-`. That
is an upstream wart, not one of ours, and not a finding.

Output is `bin/MiSTer`. A clean build takes a couple of minutes; incremental
builds of the FBUI files take a few seconds.

The build must stay **warning-free** under `-Wall -Wextra`. If you see warnings
from our files, that is a finding.

---

## 5. Running it

You almost certainly do not have a DE10-Nano with an SD card full of ROMs, and
access to the reference board is intermittent. Three options, in descending
order of fidelity:

1. **Real hardware.** Build, then use `tools/skinbake/deploy.sh` (it copies to
   `/media/fat/MiSTer.new`, then `swap` renames it, keeping a `.bak`). If the
   board's DHCP lease moved, `tools/skinbake/findmister.sh` scans the subnet.
2. **The layout preview.** Open `tools/skinpreview/preview.html` in a browser —
   it is fully self-contained (layout INI, background PNGs and a Unifont subset
   are inlined as base64). It reimplements the bitmap text engine in JavaScript,
   so it is useful for checking geometry and text metrics but is *not* the
   production code path. If you find a discrepancy between the JS
   reimplementation and the C++, that is itself a finding.
3. **Static review only.** This is a perfectly acceptable mode for this review;
   most of what we want found is visible by reading.

---

## 6. Architecture primer

The `README.md` is written in Chinese and is **partly stale** — it still
describes `fbui_theme.cpp`, which was deleted in commit `6730acd`. Use this
section instead; correcting the README is on our side, not yours.

### 6.1 Execution model — the most important constraint

FBUI is **cooperative, single-threaded, and runs on the menu main loop**.
`menu.cpp` calls `fbui_ui_hook()` immediately after `menu_key_get()`. While FBUI
is active it consumes all key events and `HandleUI()` returns early.

There is no worker thread. Anything that blocks — decoding a JPEG, scanning a
directory, waiting on a subprocess — freezes input for exactly that long. This
is why image prefetching is written as an incremental "one step per idle tick"
state machine rather than a loop. **Any suggestion that introduces a blocking
call on this path is a regression, even if the code is cleaner.**

### 6.2 Rendering pipeline

```
draw primitives ─► shadow buffer (malloc'd, cacheable DRAM)
                        │
                        ├─ dirty rectangle tracking
                        ▼
                   mmap'd HPS framebuffer plane (uncached, slow writes)
                        │
                        ▼
                   FPGA scaler ─► HDMI + VGA
```

Everything is drawn into a normal cacheable heap buffer first. Only dirty
rectangles are copied to the framebuffer mapping, because writes to that
mapping are uncached and roughly an order of magnitude slower. Correct dirty
tracking is therefore both a performance and a correctness concern — a missed
`dirty_add()` leaves stale pixels on screen.

### 6.3 Two text engines

- **240p** uses a bitmap engine in `fbui.cpp` driven by GNU Unifont. CJK glyphs
  are drawn 32 px wide by 16 px tall to compensate for the 1:2 pixel aspect
  ratio, so they appear square on the CRT. Requested sizes are quantised to
  multiples of 16 by `bitmap_scale()`.
- **1080p** uses `fbui_text.cpp` with `stb_truetype`, falling back to Unifont
  bitmaps for codepoints the TTF lacks (mostly CJK).

They are deliberately separate: antialiased small text is *less* legible than
bitmap text on a 240-line CRT.

### 6.4 Skin system

Skins are **baked offline** on a PC by `tools/skinbake/bake.py` into a
per-resolution directory of PNGs plus a `layout.ini` of absolute coordinates.
The device only decodes and blits.

This replaced an earlier design that parsed EmulationStation `theme.xml`,
rasterised SVG and rescaled large JPEGs at runtime on the ARM. The geometry in
`bake.py` intentionally mirrors `skin_defaults()` in `fbui_skin.cpp`. **That
duplication is a known smell** — if the two drift, layouts break silently. We
would welcome a concrete proposal here.

### 6.5 Memory and caching

One shared pool (`art_pool_*` in `fbui.cpp`) is split 60% to game thumbnails and
40% to system showcase art, so whichever subsystem initialises first cannot
starve the other. The pool is sized from `MemAvailable` minus a 256 MiB reserve,
capped by a skin-declared limit.

Note the real budget: MiSTer typically exposes only ~492 MiB to the HPS kernel,
not the board's nominal 1 GiB, and the scaler framebuffer lives in the same
DRAM.

### 6.6 Launching a game

FBUI writes an MGL file to `/tmp/fbui.mgl` and calls upstream's `xml_load()`.
Native core loading, ROM mounting and input routing all stay on upstream paths.

---

## 7. What we most want from this review

In rough order of value to us:

1. **Memory safety in the rasteriser and caches.** Hand-rolled span blitting,
   `size_t` byte accounting, and fixed-size `char` buffers holding filesystem
   paths. Bounds, integer overflow, and unsigned underflow are the top concern.
2. **Correctness of the cache accounting.** Slots are added and removed from
   `art_pool_used` in several places; we would like confirmation that the
   accounting cannot desynchronise from reality.
3. **Robustness against hostile or merely weird input.** ROM filenames come from
   an SD card the user fills themselves: unusual characters, very long paths,
   huge or truncated images, missing files.
4. **Blast radius of the upstream-file edits** (`video.cpp`, `osd.cpp`,
   `menu.cpp`, `file_io.cpp`). Can a user who never enables FBUI be affected?
5. **Structure.** `fbui.cpp` at 2511 lines does too many jobs. We are open to a
   proposed split, but it must not introduce blocking calls or a thread.
6. **Testability.** There is currently no seam at which anything could be
   tested. A concrete, minimal suggestion is more useful than "add tests".

---

## 8. Known concerns — confirm or expand, please do not merely re-report

We already suspect these. Telling us they exist is low value; telling us they
are worse than we think, or that a fix has a non-obvious pitfall, is high value.

- **`fbui.cpp:1196` `preview_start()` interpolates a ROM-derived filename into a
  shell command string and runs it via `popen()`.** The filename is wrapped in
  double quotes only. We believe this is injectable via a crafted filename on
  the SD card. We want to know the cleanest fix that keeps the "launch
  detached, capture the PID" behaviour.
- **`fbui.cpp:1189` `system("killall -q ffmpeg")`** kills every ffmpeg on the
  system, not just ours. It exists to reap orphans from earlier crashes.
- **`fbui.cpp` is 2511 lines** and mixes rasterising, caching, scanning, input
  and process management.
- **Geometry is duplicated** between `skin_defaults()` in `fbui_skin.cpp` and
  `bake.py` (see 6.4).
- **No tests, no CI** for our files.
- **`README.md` is stale and Chinese-only.**

---

## 9. Constraints any suggested change must respect

1. **No blocking work on the UI hook.** See 6.1.
2. **No threads.** The upstream main loop is not thread-safe and we do not want
   to be the first to assume otherwise.
3. **Keep the diff in upstream files minimal.** We rebase onto upstream
   periodically. A refactor that touches 500 lines of `menu.cpp` to save 20
   lines in ours is a net loss.
4. **Target reality.** Dual-core Cortex-A9 at 800 MHz — but see constraint 2,
   our path uses one of them — with ~492 MiB usable RAM and gnu++14. The
   codebase is C-with-classes in style and does not use exceptions or RTTI;
   please match it rather than proposing an STL-heavy rewrite.
5. **Both resolution profiles must keep working.** A change that is only
   validated at 1080p is not validated. The 240p path has different text
   rendering, different geometry, and non-square pixels.

---

## 10. What we do not want flagged

- **Chinese comments in our files.** This is deliberate and requested by the
  maintainer. Identifier names and this document are English.
- Upstream MiSTer's style, naming or architecture.
- The vendored `stb_truetype.h`.
- The choice to fork rather than upstream the feature.
- Tabs vs spaces, brace placement — we follow upstream's existing style.

---

## 11. Reporting

Please group findings by severity and give each one a file and line reference:

- **Critical** — memory corruption, code execution, data loss, or a hang
  reachable from normal use.
- **Major** — a defect a user will plausibly hit, or a regression risk to
  non-FBUI users.
- **Minor** — robustness, clarity, or maintainability.
- **Note** — observations and design suggestions.

For anything Critical or Major, a short description of how to trigger it is
worth more than a patch. We would rather understand the failure than apply a
fix we cannot reason about.
