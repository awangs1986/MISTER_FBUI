// FBUI — full-screen graphical game browser (menu core only).
//
// Rendering model:
//   - all drawing goes to a malloc'd shadow buffer (cached RAM, fast);
//   - a dirty rectangle is accumulated and only that region is copied to the
//     uncached scaler framebuffer plane (plane 1), because /dev/mem is mapped
//     O_SYNC and full-frame copies at 1080p are too slow for every keypress;
//   - the plane is shown via video_fb_enable(1, 1) — same wallpaper path the
//     menu core already uses, so OSD input routing stays untouched.
//
// Text: GNU unifont .hex file from SD (font/unifont.hex, full CJK coverage),
// falling back to the built-in 12x12 CJK samples and the 8x8 OSD font.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hardware.h"

#include "fbui.h"
#include "fbui_text.h"
#include "fbui_skin.h"
#include "file_io.h"
#include "video.h"
#include "osd.h"
#include "cfg.h"
#include "input.h"
#include "user_io.h"
#include "charrom.h"
#include "font_cjk.h"
#include "support/arcade/mra_loader.h"
#include "sxmlc.h"   // gamelist.xml only; the skin path no longer parses XML

// ---------------------------------------------------------------------------
// System -> core mapping (delay/type/index follow common MGL conventions;
// tune per-core here if a system boots without mounting the game).
// ---------------------------------------------------------------------------

enum { SYS_KIND_ROM = 0, SYS_KIND_ARCADE = 1 };

struct sys_map_t
{
	const char *dir;     // directory name under games/ (or label for Arcade)
	const char *rbf;     // MGL <rbf> value (path fragment from root)
	const char *exts;    // comma separated, lower case; NULL = all files
	char type;           // 'f' = rom file, 's' = disk/cd image
	int  index;
	int  delay;
	const char *es;      // EmulationStation system name (theme subdir)
	int  kind;           // SYS_KIND_ROM or SYS_KIND_ARCADE
};

static const sys_map_t sys_maps[] =
{
	{ "NES",       "_Console/NES",          "nes,fds,nsf",       'f', 0, 2, "nes",        SYS_KIND_ROM },
	{ "SNES",      "_Console/SNES",         "sfc,smc,bin,bs",    'f', 0, 2, "snes",       SYS_KIND_ROM },
	{ "MegaDrive", "_Console/MegaDrive",    "md,bin,gen",        'f', 1, 1, "megadrive",  SYS_KIND_ROM },
	{ "Genesis",   "_Console/MegaDrive",    "md,bin,gen",        'f', 1, 1, "megadrive",  SYS_KIND_ROM },
	{ "SMS",       "_Console/SMS",          "sms,sg,gg",         'f', 1, 1, "mastersystem", SYS_KIND_ROM },
	{ "GBA",       "_Console/GBA",          "gba",               'f', 0, 2, "gba",        SYS_KIND_ROM },
	{ "GAMEBOY",   "_Console/Gameboy",      "gb,gbc",            'f', 1, 2, "gb",         SYS_KIND_ROM },
	{ "TGFX16",    "_Console/TurboGrafx16", "pce,sgx",           'f', 0, 1, "pcengine",   SYS_KIND_ROM },
	{ "TGFX16-CD", "_Console/TurboGrafx16", "cue,chd",           's', 0, 1, "pcenginecd", SYS_KIND_ROM },
	{ "PSX",       "_Console/PSX",          "cue,chd,exe",       's', 1, 1, "psx",        SYS_KIND_ROM },
	{ "MegaCD",    "_Console/MegaCD",       "cue,chd",           's', 0, 1, "segacd",     SYS_KIND_ROM },
	{ "N64",       "_Console/N64",          "n64,z64,v64",       'f', 1, 1, "n64",        SYS_KIND_ROM },
	{ "Saturn",    "_Console/Saturn",       "cue,chd",           's', 0, 1, "saturn",     SYS_KIND_ROM },
	{ "NeoGeo",    "_Console/NeoGeo",       "neo",               'f', 1, 1, "neogeo",     SYS_KIND_ROM },
	{ "Atari2600", "_Console/Atari7800",    "a26",               'f', 1, 1, "atari2600",  SYS_KIND_ROM },
	{ "Atari7800", "_Console/Atari7800",    "a78,a26",           'f', 1, 1, "atari7800",  SYS_KIND_ROM },
	{ "AtariLynx", "_Console/AtariLynx",    "lnx",               'f', 1, 1, "lynx",       SYS_KIND_ROM },
	{ "WonderSwan","_Console/WonderSwan",   "ws,wsc",            'f', 1, 1, "wswan",      SYS_KIND_ROM },
	{ "C64",       "_Computer/C64",         "d64,t64,prg,crt",   'f', 1, 1, "c64",        SYS_KIND_ROM },
	// MRA arcade: one .mra per game under /_Arcade (CPS1/2/3, etc.)
	{ "Arcade",    "_Arcade/cores/jtcps1",  "mra",               'f', 0, 0, "arcade",     SYS_KIND_ARCADE },
};

static const sys_map_t *find_sys_map(const char *dir)
{
	for (size_t i = 0; i < sizeof(sys_maps) / sizeof(sys_maps[0]); i++)
	{
		if (!strcasecmp(sys_maps[i].dir, dir)) return &sys_maps[i];
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// unifont .hex loader (records: "XXXX:32-or-64 hex chars")
// ---------------------------------------------------------------------------

#define HEXREC_SIZE 33 // [0]=width flag (0=8px,1=16px), [1..32]=bitmap rows

static uint8_t *hex_blob = 0;
static uint32_t hex_count = 0;
static int32_t *hex_pages[256]; // BMP only: page = cp>>8, entry = record idx
static int hex_loaded = 0;      // 0=not tried, 1=ok, -1=missing

static void hexfont_load()
{
	if (hex_loaded) return;
	hex_loaded = -1;

	char path[1024];
	snprintf(path, sizeof(path), "%s/font/unifont.hex", getRootDir());
	FILE *f = fopen(path, "r");
	if (!f)
	{
		printf("FBUI: no %s, using built-in fonts\n", path);
		return;
	}

	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);

	// worst case: every ~35-byte line becomes one 33-byte record
	uint32_t max_rec = (uint32_t)(fsz / 34) + 16;
	hex_blob = (uint8_t*)malloc((size_t)max_rec * HEXREC_SIZE);
	if (!hex_blob)
	{
		fclose(f);
		return;
	}
	memset(hex_pages, 0, sizeof(hex_pages));

	char line[300];
	while (fgets(line, sizeof(line), f))
	{
		char *colon = strchr(line, ':');
		if (!colon) continue;
		uint32_t cp = (uint32_t)strtoul(line, NULL, 16);
		if (cp > 0xFFFF) continue;

		const char *hx = colon + 1;
		int hlen = 0;
		while (isxdigit((unsigned char)hx[hlen])) hlen++;
		if (hlen != 32 && hlen != 64) continue;
		if (hex_count >= max_rec) break;

		uint8_t *rec = hex_blob + (size_t)hex_count * HEXREC_SIZE;
		rec[0] = (hlen == 64) ? 1 : 0;
		int nbytes = hlen / 2;
		for (int i = 0; i < nbytes; i++)
		{
			char b[3] = { hx[i * 2], hx[i * 2 + 1], 0 };
			rec[1 + i] = (uint8_t)strtoul(b, NULL, 16);
		}

		int pg = cp >> 8;
		if (!hex_pages[pg])
		{
			hex_pages[pg] = (int32_t*)malloc(256 * sizeof(int32_t));
			if (!hex_pages[pg]) break;
			for (int i = 0; i < 256; i++) hex_pages[pg][i] = -1;
		}
		hex_pages[pg][cp & 0xFF] = (int32_t)hex_count;
		hex_count++;
	}
	fclose(f);

	hex_loaded = 1;
	printf("FBUI: loaded %u glyphs from %s\n", hex_count, path);
}

const uint8_t *fbui_hexfont_get(uint32_t cp)
{
	if (hex_loaded != 1 || cp > 0xFFFF) return NULL;
	int32_t *pg = hex_pages[cp >> 8];
	if (!pg || pg[cp & 0xFF] < 0) return NULL;
	return hex_blob + (size_t)pg[cp & 0xFF] * HEXREC_SIZE;
}

// ---------------------------------------------------------------------------
// Shadow buffer + dirty rect
// ---------------------------------------------------------------------------

static uint32_t *shadow = 0;
static uint32_t *bglayer = 0; // fully rendered static layer (gradient or theme)
static int scr_w = 0, scr_h = 0;
static int ui_lowres = 0;
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1; // x1/y1 exclusive

static void dirty_reset()
{
	dirty_x0 = scr_w; dirty_y0 = scr_h;
	dirty_x1 = 0; dirty_y1 = 0;
}

static void dirty_add(int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > scr_w) x1 = scr_w;
	if (y1 > scr_h) y1 = scr_h;
	if (x0 >= x1 || y0 >= y1) return;
	if (x0 < dirty_x0) dirty_x0 = x0;
	if (y0 < dirty_y0) dirty_y0 = y0;
	if (x1 > dirty_x1) dirty_x1 = x1;
	if (y1 > dirty_y1) dirty_y1 = y1;
}

static void flush_to_plane()
{
	if (dirty_x0 >= dirty_x1 || dirty_y0 >= dirty_y1) return;

	int w, h;
	uint32_t *plane = video_fb_get_plane(1, &w, &h);
	if (!plane || w != scr_w || h != scr_h) return;

	int span = (dirty_x1 - dirty_x0) * 4;
	for (int y = dirty_y0; y < dirty_y1; y++)
	{
		memcpy(plane + y * scr_w + dirty_x0, shadow + y * scr_w + dirty_x0, span);
	}
	dirty_reset();
}

// ---------------------------------------------------------------------------
// Drawing primitives (into shadow)
// ---------------------------------------------------------------------------

static void fill_rect(int x, int y, int w, int h, uint32_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > scr_w) w = scr_w - x;
	if (y + h > scr_h) h = scr_h - y;
	if (w <= 0 || h <= 0) return;

	for (int j = 0; j < h; j++)
	{
		uint32_t *p = shadow + (y + j) * scr_w + x;
		for (int i = 0; i < w; i++) *p++ = color;
	}
	dirty_add(x, y, x + w, y + h);
}

// Alpha-composite a solid color over the current shadow contents.
static void blend_rect(int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
	if (!alpha) return;
	if (alpha == 255) { fill_rect(x, y, w, h, color); return; }
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > scr_w) w = scr_w - x;
	if (y + h > scr_h) h = scr_h - y;
	if (w <= 0 || h <= 0) return;

	uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
	uint32_t inv = 255 - alpha;
	for (int j = 0; j < h; j++)
	{
		uint32_t *p = shadow + (y + j) * scr_w + x;
		for (int i = 0; i < w; i++, p++)
		{
			uint32_t d = *p;
			uint32_t r = (sr * alpha + ((d >> 16) & 0xFF) * inv) / 255;
			uint32_t g = (sg * alpha + ((d >> 8) & 0xFF) * inv) / 255;
			uint32_t b = (sb * alpha + (d & 0xFF) * inv) / 255;
			*p = (r << 16) | (g << 8) | b;
		}
	}
	dirty_add(x, y, x + w, y + h);
}

// copy a region of the static background layer back into the shadow buffer
static void bg_restore(int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > scr_w) w = scr_w - x;
	if (y + h > scr_h) h = scr_h - y;
	if (w <= 0 || h <= 0) return;

	for (int j = 0; j < h; j++)
		memcpy(shadow + (y + j) * scr_w + x, bglayer + (y + j) * scr_w + x, (size_t)w * 4);
	dirty_add(x, y, x + w, y + h);
}

// ---------------------------------------------------------------------------
// Bitmap text engine
// ---------------------------------------------------------------------------
// Glyph rows are turned into horizontal runs and written straight into the
// shadow buffer. Clipping is resolved per run and the dirty rectangle is
// extended once per string, rather than once per lit pixel: a screen of CJK
// text used to cost roughly eleven thousand fill_rect/dirty_add pairs.

// Horizontal multiplier for full-width (CJK) cells. At 640x240 a framebuffer
// pixel is half as wide as it is tall, so a 16x16 glyph reaches the eye
// squashed to half width and the strokes merge into a blob. Doubling the width
// makes CJK square again; Latin keeps single width, which is exactly the
// half-width / full-width convention.
static inline int cjk_xscale(void)
{
	return ui_lowres ? 2 : 1;
}

// One horizontal run. Caller guarantees y is inside the buffer.
static inline void text_span(int x, int y, int n, uint32_t color)
{
	if (x < 0) { n += x; x = 0; }
	if (x + n > scr_w) n = scr_w - x;
	if (n <= 0) return;
	uint32_t *p = shadow + (size_t)y * scr_w + x;
	while (n--) *p++ = color;
}

// Unclipped-in-y rect used for glyph decorations; no dirty bookkeeping.
static void text_rect(int x, int y, int w, int h, uint32_t color)
{
	for (int j = 0; j < h; j++)
	{
		int fy = y + j;
		if ((unsigned)fy < (unsigned)scr_h) text_span(x, fy, w, color);
	}
}

// Emit one glyph row. `bits` holds `nbits` pixels MSB-first; each source pixel
// becomes sx wide and sy tall. Adjacent set bits collapse into a single run.
static void glyph_row(int x, int y, uint32_t bits, int nbits, int sx, int sy, uint32_t color)
{
	if (!bits) return;
	for (int col = 0; col < nbits; )
	{
		if (!(bits & (1u << (nbits - 1 - col)))) { col++; continue; }
		int run = 1;
		while (col + run < nbits && (bits & (1u << (nbits - 1 - col - run)))) run++;
		for (int j = 0; j < sy; j++)
		{
			int fy = y + j;
			if ((unsigned)fy < (unsigned)scr_h) text_span(x + col * sx, fy, run * sx, color);
		}
		col += run;
	}
}

// pack a column-major 8x8 charfont row into an MSB-first bit mask
static inline uint32_t charfont_row(const unsigned char *g, int row)
{
	uint32_t bits = 0;
	for (int col = 0; col < 8; col++)
		if (g[col] & (1 << row)) bits |= 0x80u >> col;
	return bits;
}

// Advance width of one glyph. Must agree with draw_glyph exactly.
static int glyph_adv(uint32_t cp, int s)
{
	if (ui_lowres && cp < 0x80) return 16;
	const uint8_t *rec = fbui_hexfont_get(cp);
	int wide = rec ? rec[0] : (cp >= 0x80);
	return wide ? (16 * cjk_xscale() * s) : (8 * s);
}

// scale s: glyph cell is 16*s px tall. Returns x advance.
static int draw_glyph(int x, int y, uint32_t cp, uint32_t color, int s)
{
	int xs = cjk_xscale();

	// Dedicated 240p Latin face: MiSTer's built-in 8x8 font at exact 2x.
	// It shares a 16x16 cell with native-size Unifont CJK glyphs.
	if (ui_lowres && cp < 0x80)
	{
		const unsigned char *g = charfont[cp & 0x7F];
		for (int row = 0; row < 8; row++)
			glyph_row(x, y + row * 2, charfont_row(g, row), 8, 2, 2, color);
		return 16;
	}

	const uint8_t *rec = fbui_hexfont_get(cp);
	if (rec)
	{
		int wide = rec[0];
		const uint8_t *bm = rec + 1;
		int nbits = wide ? 16 : 8;
		int sx = (wide ? xs : 1) * s;
		for (int row = 0; row < 16; row++)
		{
			uint32_t bits = wide ? (((uint32_t)bm[row * 2] << 8) | bm[row * 2 + 1]) : bm[row];
			glyph_row(x, y + row * s, bits, nbits, sx, s, color);
		}
		return nbits * sx;
	}

	// built-in 12x12 CJK sample (column-major, bit0 = top), centered in 16px
	const uint16_t *cols = (cp >= 0x80) ? font_cjk_get(cp) : NULL;
	if (cols)
	{
		int sx = xs * s;
		for (int row = 0; row < FONT_CJK_HEIGHT; row++)
		{
			uint32_t bits = 0;
			for (int col = 0; col < FONT_CJK_WIDTH; col++)
				if (cols[col] & (1 << row)) bits |= 1u << (FONT_CJK_WIDTH - 1 - col);
			glyph_row(x + 2 * sx, y + (row + 2) * s, bits, FONT_CJK_WIDTH, sx, s, color);
		}
		return 16 * sx;
	}

	if (cp < 0x80)
	{
		// built-in OSD 8x8 font (column-major, bit0 = top), doubled to 8x16 cell
		const unsigned char *g = charfont[cp & 0x7F];
		for (int row = 0; row < 8; row++)
			glyph_row(x, y + (row * 2 + 1) * s, charfont_row(g, row), 8, s, 2 * s, color);
		return 8 * s;
	}

	// missing glyph: hollow box
	int sx = xs * s;
	int w = 14 * sx;
	text_rect(x + sx, y + 2 * s, w, s, color);
	text_rect(x + sx, y + 13 * s, w, s, color);
	text_rect(x + sx, y + 2 * s, sx, 12 * s, color);
	text_rect(x + w, y + 2 * s, sx, 12 * s, color);
	return 16 * sx;
}

// Draw UTF-8 string, clipped to max_w px. Returns drawn width.
static int draw_text(int x, int y, const char *str, uint32_t color, int s, int max_w)
{
	int cx = 0;
	const char *p = str;
	while (*p)
	{
		uint32_t cp;
		p += utf8_decode(p, &cp);
		int adv = glyph_adv(cp, s);
		if (max_w && cx + adv > max_w)
		{
			// ellipsis
			if (max_w >= cx + 6 * s)
				for (int i = 0; i < 3; i++)
					text_rect(x + cx + i * 2 * s + s, y + (ui_lowres ? 14 : 14 * s), s, s, color);
			break;
		}
		draw_glyph(x + cx, y, cp, color, s);
		cx += adv;
	}

	// The HD 8x8 fallback face reaches 17*s below the origin; be generous
	// rather than leave a row of stale pixels un-flushed.
	if (cx > 0) dirty_add(x, y, x + cx, y + (ui_lowres ? 16 : 18 * s));
	return cx;
}

static int text_width(const char *str, int s)
{
	int w = 0;
	const char *p = str;
	while (*p)
	{
		uint32_t cp;
		p += utf8_decode(p, &cp);
		w += glyph_adv(cp, s);
	}
	return w;
}

// ---------------------------------------------------------------------------
// Browser state
// ---------------------------------------------------------------------------

#define FBUI_MAX_ITEMS 4096
#define NAME_LEN 256

struct entry_t
{
	char name[NAME_LEN]; // on-disk filename (No-Intro English)
	char disp[NAME_LEN]; // UI label (gamelist <name>, else same as name)
	uint8_t is_dir;
};

enum { LV_SYSTEMS = 0, LV_GAMES };

static int ui_active = 0;
static int ui_level = LV_SYSTEMS;
static entry_t *items = 0;
static int item_cnt = 0;
static int sel = 0, scroll_top = 0;
static char cur_sys[NAME_LEN];        // current system dir name
static char cur_rel[1024];            // path inside the system dir ("" = root)
static const sys_map_t *cur_map = 0;
static int need_full_redraw = 0;
static int need_list_redraw = 0;
static int need_panel_redraw = 0;
static uint32_t showcase_hold = 0;    // debounce: full art redraw after input settles
static uint32_t nav_held = 0;         // last nav key still held (FBUI-local repeat)
static uint32_t nav_repeat_at = 0;
static int nav_busy = 0;             // 1 while doing heavy decode — drop nav until release
static char msg_text[128];            // transient message shown in footer
static uint32_t msg_timer = 0;

static void preview_stop(void);
static void sc_prefetch_reset(void);
static void th_cache_reset(void);
static void th_cache_clear_all(void);
static void th_cache_free(void);
static int  th_prefetch_step(void);
static int  thumb_draw(const char *path, int bx, int by, int bw, int bh);
static pid_t prev_pid = -1;

// Layout mirrors of the active skin. The skin engine owns every number; these
// are just locals so the render code below stays readable.
static int ui_scale;
static int header_h, footer_h, row_h, list_x, list_y, list_w, list_rows;
static int panel_x, panel_y, panel_w, panel_h;
static int game_backdrop_active;
// CRTs commonly overscan. Keep the complete 240p composition inside this inset.
static int safe_l, safe_t, safe_r, safe_b;

// Active style, entirely skin-driven (the skin engine supplies built-in
// defaults when no package is installed, so this is always populated).
static struct
{
	int showcase;     // system-view style: one system, full-bleed art
	uint32_t text, dim, sel_bg, sel_text, accent;
	int font_px;      // list row text height
	int list_align;   // SKIN_ALIGN_*
	int have_panel;   // a media panel slot exists at this level
} st;

static void layout_compute()
{
	const skin_t *s = skin();

	// The resolution profile is decided once, inside the skin engine. Nothing
	// else in this file may re-derive it from scr_h: a mode switch and the FB
	// remap do not become visible atomically, so geometry must come from a
	// single snapshot taken when the skin was loaded.
	ui_lowres = s->lowres;
	ui_scale = s->scale;
	safe_l = s->safe_l;
	safe_t = s->safe_t;
	safe_r = s->safe_r;
	safe_b = s->safe_b;

	header_h = s->header_h;
	footer_h = s->footer_h;
	row_h = s->row_h;
	list_x = s->list_x;
	list_y = s->list_y;
	list_w = s->list_w;
	list_rows = s->list_rows;

	// One panel rect for both levels. The static layer is pre-baked, so a
	// position that moved with the browser level would no longer line up with
	// the artwork underneath it.
	panel_x = s->panel_x;
	panel_y = s->panel_y;
	panel_w = s->panel_w;
	panel_h = s->panel_h;
}

// ES system name for a games/ directory (sys_maps entry or lowercased name)
static const char *es_sys_name(const char *dir)
{
	const sys_map_t *m = find_sys_map(dir);
	if (m && m->es) return m->es;
	static char low[NAME_LEN];
	snprintf(low, sizeof(low), "%s", dir);
	for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
	return low;
}

// Apply the active skin to the style block. Colors, fonts, the showcase switch
// and every coordinate come from the package, or from the skin engine's
// built-in defaults when no package is installed.
static void skin_apply()
{
	const skin_t *s = skin();

	// Showcase is a systems-level presentation only; game lists always need
	// the text list and media panel.
	st.showcase = s->showcase && (ui_level == LV_SYSTEMS);
	st.have_panel = 1;
	st.text = s->col_text;
	st.dim = s->col_dim;
	st.sel_bg = s->col_sel_bar;
	st.sel_text = s->col_sel_text;
	st.accent = s->col_accent;
	st.font_px = s->font_px;
	st.list_align = s->list_align;

	// 240p is bitmap-only. A hinted TrueType face at 16px on a 240-line CRT is
	// less legible than the unifont cells and has worse CJK coverage.
	fbui_ttf_set_font(s->lowres ? NULL : s->font);
}

static void layout_apply()
{
	layout_compute();
	if (st.showcase)
	{
		// single-item carousel: no on-screen list geometry needed
		list_rows = 1;
		list_x = 0;
		list_y = 0;
		list_w = 0;
		row_h = scr_h;
		st.have_panel = 0;
	}
}

// The bitmap face only exists at multiples of its 16px design size, so a
// requested pixel height snaps to the nearest whole multiple. Rounding down
// instead would drop 1080p's 25px footer to 16px — half the body text rather
// than the intended 80% — and cap the 42px title at 32px.
static inline int bitmap_scale(int px)
{
	if (ui_lowres) return 1;
	int s = (px + 8) / 16;
	return s < 1 ? 1 : s;
}

// text dispatch: TrueType when the skin supplies one, bitmap engine otherwise
static int ui_text(int x, int y, const char *s, uint32_t color, int px, int max_w, int align)
{
	if (!ui_lowres && fbui_ttf_available())
	{
		int w = fbui_ttf_draw(shadow, scr_w, scr_h, x, y, s, color, px, max_w, align);
		dirty_add(x, y, x + ((max_w > 0) ? max_w : w), y + px + px / 4 + 1);
		return w;
	}

	int sc = bitmap_scale(px);
	if (max_w > 0 && align != SKIN_ALIGN_LEFT)
	{
		int tw = text_width(s, sc);
		if (tw < max_w) x += (align == SKIN_ALIGN_CENTER) ? (max_w - tw) / 2 : (max_w - tw);
	}
	return draw_text(x, y, s, color, sc, max_w);
}

// fill a rect in the static layer (bglayer), not the shadow buffer
static void bg_fill(int x, int y, int w, int h, uint32_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > scr_w) w = scr_w - x;
	if (y + h > scr_h) h = scr_h - y;
	if (w <= 0 || h <= 0) return;

	for (int j = 0; j < h; j++)
	{
		uint32_t *p = bglayer + (size_t)(y + j) * scr_w + x;
		for (int i = 0; i < w; i++) *p++ = color;
	}
}

// Build the complete static layer: either the skin's pre-composited bg.png
// (decoded once, cached, then a straight memcpy), or an equivalent composition
// drawn here. Everything downstream only ever calls bg_restore, so there is a
// single drawing path whether or not a package is installed.
static void render_bglayer()
{
	const skin_t *s = skin();
	if (skin_fill_background(bglayer, scr_w, scr_h)) return;

	int r0 = (s->col_bg_top >> 16) & 0xFF, g0 = (s->col_bg_top >> 8) & 0xFF, b0 = s->col_bg_top & 0xFF;
	int r1 = (s->col_bg_bot >> 16) & 0xFF, g1 = (s->col_bg_bot >> 8) & 0xFF, b1 = s->col_bg_bot & 0xFF;
	for (int y = 0; y < scr_h; y++)
	{
		int r = r0 + (r1 - r0) * y / scr_h;
		int g = g0 + (g1 - g0) * y / scr_h;
		int b = b0 + (b1 - b0) * y / scr_h;
		uint32_t c = (uint32_t)((r << 16) | (g << 8) | b);
		uint32_t *p = bglayer + (size_t)y * scr_w;
		for (int x = 0; x < scr_w; x++) *p++ = c;
	}

	if (st.showcase) return; // full-bleed system art is painted over this

	int bx = safe_l;
	int bw = scr_w - safe_l - safe_r;
	int accent_h = 2 * ui_scale;
	bg_fill(bx, safe_t, bw, header_h, s->col_header);
	bg_fill(bx, safe_t + header_h - accent_h, bw, accent_h, s->col_accent);
	bg_fill(bx, scr_h - safe_b - footer_h, bw, footer_h, s->col_header);
	if (st.have_panel)
	{
		bg_fill(panel_x, panel_y, panel_w, panel_h, s->col_panel);
		bg_fill(panel_x, panel_y, panel_w, accent_h, s->col_accent);
	}
}

// ---------------------------------------------------------------------------
// Directory scanning
// ---------------------------------------------------------------------------

static int ext_matches(const char *name, const char *exts)
{
	if (!exts) return 1;
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1]) return 0;

	char ext[16];
	snprintf(ext, sizeof(ext), "%s", dot + 1);
	for (char *c = ext; *c; c++) *c = (char)tolower((unsigned char)*c);

	const char *p = exts;
	size_t elen = strlen(ext);
	while (*p)
	{
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		if (len == elen && !strncasecmp(p, ext, len)) return 1;
		if (!comma) break;
		p = comma + 1;
	}
	return 0;
}

// MiSTer cores are named Core.rbf or Core_YYYYMMDD.rbf (same rule as MGL).
static int core_rbf_exists(const char *rbf_fragment)
{
	if (!rbf_fragment || !rbf_fragment[0]) return 0;

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/%s", getRootDir(), rbf_fragment);
	char *slash = strrchr(dir, '/');
	if (!slash) return 0;
	*slash = 0;
	const char *prefix = slash + 1;
	size_t plen = strlen(prefix);
	if (!plen) return 0;

	DIR *d = opendir(dir);
	if (!d) return 0;

	int found = 0;
	struct dirent *de;
	while ((de = readdir(d)))
	{
		size_t len = strlen(de->d_name);
		if (len < plen + 4) continue;
		if (strcasecmp(de->d_name + len - 4, ".rbf")) continue;
		if (strncasecmp(de->d_name, prefix, plen)) continue;
		if (de->d_name[plen] != '.' && de->d_name[plen] != '_') continue;
		found = 1;
		break;
	}
	closedir(d);
	return found;
}

static int system_has_roms(const char *sys_path, const sys_map_t *map)
{
	if (!map) return 0;
	DIR *d = opendir(sys_path);
	if (!d) return 0;

	int found = 0;
	struct dirent *de;
	while ((de = readdir(d)))
	{
		if (de->d_name[0] == '.') continue;

		int is_dir = (de->d_type == DT_DIR);
		if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK)
		{
			char full[2048];
			snprintf(full, sizeof(full), "%s/%s", sys_path, de->d_name);
			struct stat st;
			if (stat(full, &st)) continue;
			is_dir = S_ISDIR(st.st_mode);
		}
		if (is_dir) continue;
		if (ext_matches(de->d_name, map->exts))
		{
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

static int arcade_cores_present(void)
{
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/_Arcade/cores", getRootDir());
	DIR *d = opendir(dir);
	if (!d) return 0;
	int found = 0;
	struct dirent *de;
	while ((de = readdir(d)))
	{
		size_t len = strlen(de->d_name);
		if (len > 4 && !strcasecmp(de->d_name + len - 4, ".rbf"))
		{
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

static int arcade_has_mra(void)
{
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/_Arcade", getRootDir());
	DIR *d = opendir(dir);
	if (!d) return 0;
	int found = 0;
	struct dirent *de;
	while ((de = readdir(d)))
	{
		if (de->d_name[0] == '.' || de->d_name[0] == '_') continue;
		size_t len = strlen(de->d_name);
		if (len > 4 && !strcasecmp(de->d_name + len - 4, ".mra"))
		{
			found = 1;
			break;
		}
	}
	closedir(d);
	return found;
}

static int entry_cmp(const void *a, const void *b);

static void scan_arcade(void)
{
	item_cnt = 0;
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/_Arcade", getRootDir());
	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *de;
	while ((de = readdir(d)) && item_cnt < FBUI_MAX_ITEMS)
	{
		if (de->d_name[0] == '.' || de->d_name[0] == '_') continue;
		size_t len = strlen(de->d_name);
		if (len <= 4 || strcasecmp(de->d_name + len - 4, ".mra")) continue;

		int is_dir = (de->d_type == DT_DIR);
		if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK)
		{
			char full[2048];
			snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
			struct stat st;
			if (stat(full, &st)) continue;
			is_dir = S_ISDIR(st.st_mode);
		}
		if (is_dir) continue;

		entry_t *e = &items[item_cnt++];
		snprintf(e->name, sizeof(e->name), "%s", de->d_name);
		// display without .mra
		snprintf(e->disp, sizeof(e->disp), "%s", de->d_name);
		if (len > 4) e->disp[len - 4] = 0;
		e->is_dir = 0;
	}
	closedir(d);
	qsort(items, item_cnt, sizeof(entry_t), entry_cmp);
	sel = 0;
	scroll_top = 0;
}

static void maybe_add_arcade_system(void)
{
	if (!arcade_cores_present() || !arcade_has_mra()) return;
	for (int i = 0; i < item_cnt; i++)
		if (!strcasecmp(items[i].name, "Arcade")) return;
	if (item_cnt >= FBUI_MAX_ITEMS) return;

	entry_t *e = &items[item_cnt++];
	snprintf(e->name, sizeof(e->name), "Arcade");
	snprintf(e->disp, sizeof(e->disp), "街机 Arcade");
	e->is_dir = 1;
	qsort(items, item_cnt, sizeof(entry_t), entry_cmp);
}

static int entry_cmp(const void *a, const void *b)
{
	const entry_t *ea = (const entry_t*)a, *eb = (const entry_t*)b;
	if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
	return strcasecmp(ea->name, eb->name);
}

static void games_root(char *out, size_t sz)
{
	snprintf(out, sz, "%s/%s", getRootDir(), GAMES_DIR);
}

static void cur_dir_path(char *out, size_t sz)
{
	char root[1024];
	games_root(root, sizeof(root));
	if (cur_rel[0]) snprintf(out, sz, "%s/%s/%s", root, cur_sys, cur_rel);
	else snprintf(out, sz, "%s/%s", root, cur_sys);
}

static void scan_dir(const char *path, int games_level)
{
	item_cnt = 0;

	DIR *d = opendir(path);
	if (!d) return;

	struct dirent *de;
	while ((de = readdir(d)) && item_cnt < FBUI_MAX_ITEMS)
	{
		if (de->d_name[0] == '.') continue;

		int is_dir = (de->d_type == DT_DIR);
		if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK)
		{
			char full[2048];
			snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
			struct stat st;
			if (stat(full, &st)) continue;
			is_dir = S_ISDIR(st.st_mode);
		}

		if (games_level)
		{
			if (is_dir)
			{
				// skip cover/artwork folders in the list
				if (!strcasecmp(de->d_name, "covers") ||
				    !strcasecmp(de->d_name, "images") ||
				    !strcasecmp(de->d_name, "videos") ||
				    !strcasecmp(de->d_name, "media")) continue;
			}
			else if (cur_map && !ext_matches(de->d_name, cur_map->exts)) continue;
		}
		else
		{
			// systems level: only dirs that have a mapped core + at least one ROM
			if (!is_dir) continue;
			const sys_map_t *m = find_sys_map(de->d_name);
			if (!m) continue;
			if (m->kind == SYS_KIND_ARCADE) continue; // injected via maybe_add_arcade_system
			if (!core_rbf_exists(m->rbf)) continue;
			char full[2048];
			snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
			if (!system_has_roms(full, m)) continue;
		}

		entry_t *e = &items[item_cnt++];
		snprintf(e->name, sizeof(e->name), "%s", de->d_name);
		snprintf(e->disp, sizeof(e->disp), "%s", de->d_name);
		e->is_dir = (uint8_t)is_dir;
	}
	closedir(d);

	qsort(items, item_cnt, sizeof(entry_t), entry_cmp);
	sel = 0;
	scroll_top = 0;
}

static int entry_find_file(const char *filename)
{
	// NOTE: items are sorted dirs-first then by name, so binary search by
	// name alone is invalid whenever any subdirectory is present (e.g. NES/Palettes).
	// Owned libraries are small — linear scan is fine and correct.
	for (int i = 0; i < item_cnt; i++)
	{
		if (items[i].is_dir) continue;
		if (!strcasecmp(items[i].name, filename)) return i;
	}
	return -1;
}

static void gl_xml_text(const XMLNode *n, char *out, size_t sz)
{
	out[0] = 0;
	if (!n) return;
	if (n->text) { snprintf(out, sz, "%s", n->text); return; }
	for (int i = 0; i < n->n_children; i++)
	{
		if (n->children[i]->tag_type == TAG_TEXT && n->children[i]->text)
		{
			snprintf(out, sz, "%s", n->children[i]->text);
			return;
		}
	}
}

// Apply <name> from gamelist.xml onto matching on-disk entries (display only).
static void gamelist_apply_names(const char *dir)
{
	char xml[2100];
	snprintf(xml, sizeof(xml), "%s/gamelist.xml", dir);
	if (access(xml, R_OK)) return;

	XMLDoc doc;
	XMLDoc_init(&doc);
	if (!XMLDoc_parse_file_DOM_text_as_nodes(xml, &doc, 0) || doc.i_root < 0)
	{
		XMLDoc_free(&doc);
		return;
	}

	int applied = 0;
	XMLNode *root = doc.nodes[doc.i_root];
	for (int i = 0; i < root->n_children; i++)
	{
		XMLNode *g = root->children[i];
		if (!g->tag || strcasecmp(g->tag, "game")) continue;

		char path[512] = "", name[NAME_LEN] = "";
		for (int j = 0; j < g->n_children; j++)
		{
			XMLNode *c = g->children[j];
			if (!c->tag) continue;
			char val[512];
			gl_xml_text(c, val, sizeof(val));
			if (!strcasecmp(c->tag, "path")) snprintf(path, sizeof(path), "%s", val);
			else if (!strcasecmp(c->tag, "name")) snprintf(name, sizeof(name), "%s", val);
		}
		if (!path[0] || !name[0]) continue;

		const char *bn = strrchr(path, '/');
		bn = bn ? bn + 1 : path;
		if (bn[0] == '.' && bn[1] == '/') bn += 2;

		int idx = entry_find_file(bn);
		if (idx < 0 || items[idx].is_dir) continue;
		snprintf(items[idx].disp, sizeof(items[idx].disp), "%s", name);
		applied++;
	}
	XMLDoc_free(&doc);
	if (applied)
		printf("FBUI: gamelist names applied: %d\n", applied);
}

// The skin is parsed once per FBUI start (the output resolution cannot change
// mid-session), so switching browser level only re-derives the style and
// geometry from the already-loaded package. No file I/O happens here.
static void style_refresh()
{
	skin_apply();
	layout_apply();
}

static void enter_systems()
{
	preview_stop();
	ui_level = LV_SYSTEMS;
	cur_sys[0] = 0;
	cur_rel[0] = 0;
	cur_map = 0;
	char root[1024];
	games_root(root, sizeof(root));
	scan_dir(root, 0);
	maybe_add_arcade_system();
	style_refresh();
	sc_prefetch_reset();
	need_full_redraw = 1;
}

static void enter_games()
{
	preview_stop();
	ui_level = LV_GAMES;
	if (cur_map && cur_map->kind == SYS_KIND_ARCADE)
	{
		scan_arcade();
		char adir[1024];
		snprintf(adir, sizeof(adir), "%s/_Arcade", getRootDir());
		gamelist_apply_names(adir);
		style_refresh();
		// The previous directory's covers can never be hit again, and holding
		// them would only shrink the window available to this list.
		th_cache_clear_all();
		th_cache_reset();
		need_full_redraw = 1;
		return;
	}
	char path[2048];
	cur_dir_path(path, sizeof(path));
	scan_dir(path, 1);
	gamelist_apply_names(path);
	style_refresh();
	th_cache_clear_all();
	th_cache_reset();
	need_full_redraw = 1;
}

// ---------------------------------------------------------------------------
// Cover / video art
// Convention: games/<sys>/images/<rom_basename>.png
//             games/<sys>/videos/<rom_basename>.mp4
// Missing files => empty preview (no placeholder text).
// ---------------------------------------------------------------------------

// Look up image/video for one ROM by on-disk convention. Either output may
// be left empty if the file is absent.
static void find_media(const char *game, char *image_out, size_t isz,
                       char *video_out, size_t vsz)
{
	if (image_out && isz) image_out[0] = 0;
	if (video_out && vsz) video_out[0] = 0;

	char dir[2048], base[NAME_LEN];
	if (cur_map && cur_map->kind == SYS_KIND_ARCADE)
		snprintf(dir, sizeof(dir), "%s/_Arcade", getRootDir());
	else
		cur_dir_path(dir, sizeof(dir));
	snprintf(base, sizeof(base), "%s", game);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;

	char cand[2400];
	if (image_out && isz)
	{
		static const char *ipats[] = {
			"%s/images/%s.png", "%s/images/%s.jpg",
			"%s/media/images/%s.png", "%s/media/images/%s.jpg",
		};
		for (size_t i = 0; i < sizeof(ipats) / sizeof(ipats[0]); i++)
		{
			snprintf(cand, sizeof(cand), ipats[i], dir, base);
			if (!access(cand, R_OK)) { snprintf(image_out, isz, "%s", cand); break; }
		}
	}
	if (video_out && vsz)
	{
		static const char *vpats[] = {
			"%s/videos/%s.mp4", "%s/videos/%s.mkv", "%s/videos/%s.avi",
			"%s/media/videos/%s.mp4",
		};
		for (size_t i = 0; i < sizeof(vpats) / sizeof(vpats[0]); i++)
		{
			snprintf(cand, sizeof(cand), vpats[i], dir, base);
			if (!access(cand, R_OK)) { snprintf(video_out, vsz, "%s", cand); break; }
		}
	}
	// Media is resolved by basename under images/ and videos/. The generated
	// gamelist.xml documents the same paths; we do not re-parse the (often
	// multi-thousand-entry) XML on every selection.
}

// Draw the selected game's screenshot behind the browser. Scale uniformly to
// fit the main safe viewport: never stretch or crop, and leave black bars when
// the source and display aspect ratios differ.
static int render_game_backdrop_layer(void)
{
	if (ui_level != LV_GAMES || !item_cnt || sel < 0 || sel >= item_cnt || items[sel].is_dir)
		return 0;

	char image[2400] = "";
	find_media(items[sel].name, image, sizeof(image), NULL, 0);
	if (!image[0]) return 0;

	int x = safe_l;
	int y = safe_t;
	int w = scr_w - safe_l - safe_r;
	int h = scr_h - safe_t - safe_b - footer_h - 4;
	if (w <= 0 || h <= 0) return 0;
	for (int row = 0; row < h; row++)
	{
		uint32_t *p = bglayer + (size_t)(y + row) * scr_w + x;
		for (int col = 0; col < w; col++) p[col] = 0x000000;
	}
	// 640x240 is presented as 4:3 on a CRT: framebuffer pixels are about half
	// as wide as they are tall. The skin's pixel aspect corrects that, so a
	// square-pixel 4:3 screenshot fills the screen instead of a narrow strip.
	return skin_img_blit_file(bglayer, scr_w, scr_h, image, x, y, w, h, 1);
}

// ---------------------------------------------------------------------------
// Video preview (ffmpeg soft-decode → /tmp/fbui_prev.png, then blit)
// ---------------------------------------------------------------------------

static char prev_src[2400] = "";
static int prev_bx, prev_by, prev_bw, prev_bh;

static void preview_stop(void)
{
	if (prev_pid > 0)
	{
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "kill %d 2>/dev/null; kill -9 %d 2>/dev/null", (int)prev_pid, (int)prev_pid);
		system(cmd);
		// also reap any orphaned ffmpeg left from a previous crash
		system("killall -q ffmpeg 2>/dev/null");
		prev_pid = -1;
	}
	prev_src[0] = 0;
	unlink("/tmp/fbui_prev.png");
}

static void preview_start(const char *video, int bx, int by, int bw, int bh)
{
	if (!video || !video[0] || access(video, R_OK)) { preview_stop(); return; }
	if (prev_pid > 0 && !strcmp(prev_src, video) &&
	    bx == prev_bx && by == prev_by && bw == prev_bw && bh == prev_bh)
		return;

	preview_stop();
	snprintf(prev_src, sizeof(prev_src), "%s", video);
	prev_bx = bx; prev_by = by; prev_bw = bw; prev_bh = bh;

	// looped soft-decode; overwrite a single PNG that FBUI blits each refresh
	char cmd[2800];
	snprintf(cmd, sizeof(cmd),
		"ffmpeg -nostdin -hide_banner -loglevel error -stream_loop -1 -re -i \"%s\" "
		"-vf scale=%d:%d:force_original_aspect_ratio=decrease "
		"-r 12 -update 1 -y /tmp/fbui_prev.png >/dev/null 2>&1 & echo $!",
		video, bw > 8 ? bw : 320, bh > 8 ? bh : 240);

	FILE *p = popen(cmd, "r");
	if (!p) return;
	char line[32] = "";
	if (fgets(line, sizeof(line), p)) prev_pid = (pid_t)atoi(line);
	pclose(p);
}

static int preview_blit(int bx, int by, int bw, int bh)
{
	if (access("/tmp/fbui_prev.png", R_OK)) return 0;
	skin_img_blit_file(shadow, scr_w, scr_h, "/tmp/fbui_prev.png", bx, by, bw, bh, 0);
	dirty_add(bx, by, bx + bw, by + bh);
	return 1;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// width of a string in the engine ui_text would actually use
static int ui_text_width(const char *s, int px)
{
	if (!ui_lowres && fbui_ttf_available()) return fbui_ttf_width(s, px);
	return text_width(s, bitmap_scale(px));
}

static void render_header()
{
	if (st.showcase) return; // counter drawn inside render_showcase

	const skin_t *s = skin();
	int hx = safe_l;
	int hy = safe_t;
	int hw = scr_w - safe_l - safe_r;
	int pad = s->row_pad;

	// The band itself lives in the static layer, so only the text is repainted.
	bg_restore(hx, hy, hw, header_h);

	int tpx = s->title_px;
	int ty = hy + (header_h - tpx) / 2;
	if (ty < hy) ty = hy;

	char title[300];
	if (ui_level == LV_SYSTEMS) snprintf(title, sizeof(title), "游戏库 GAME LIBRARY");
	else if (cur_rel[0]) snprintf(title, sizeof(title), "%s / %s", cur_sys, cur_rel);
	else snprintf(title, sizeof(title), "%s", cur_sys);

	char cnt[48];
	snprintf(cnt, sizeof(cnt), "%d / %d", item_cnt ? sel + 1 : 0, item_cnt);
	int cw = ui_text_width(cnt, s->font_px);

	int tw = hw - cw - 3 * pad;
	if (tw > 0) ui_text(hx + pad, ty, title, s->col_title, tpx, tw, SKIN_ALIGN_LEFT);
	ui_text(hx + hw - cw - pad, hy + (header_h - s->font_px) / 2, cnt, st.dim,
		s->font_px, cw, SKIN_ALIGN_RIGHT);
}

static void render_footer()
{
	int show_msg_now = (msg_text[0] && !CheckTimer(msg_timer));
	if (!show_msg_now) msg_text[0] = 0;

	const skin_t *s = skin();
	int fx = safe_l;
	int fy = scr_h - safe_b - footer_h;
	int fw = scr_w - safe_l - safe_r;
	int fpx = s->footer_px;
	int pad = s->row_pad;

	bg_restore(fx, fy, fw, footer_h);

	int ty = fy + (footer_h - fpx) / 2;
	if (ty < fy) ty = fy;

	if (show_msg_now)
	{
		ui_text(fx + pad, ty, msg_text, s->col_msg, fpx, fw - 2 * pad, SKIN_ALIGN_LEFT);
		return;
	}

	const char *hint = ui_lowres
		? ((ui_level == LV_SYSTEMS) ? "A 进入   START 菜单" : "A 启动   B 返回   START 菜单")
		: ((ui_level == LV_SYSTEMS) ? "ENTER 进入   F12/ESC 经典菜单"
		                              : "ENTER 启动   BACKSPACE 返回   F12/ESC 经典菜单");
	ui_text(fx + pad, ty, hint, st.dim, fpx, fw - 2 * pad, SKIN_ALIGN_LEFT);
}

static void render_row(int row)
{
	int idx = scroll_top + row;
	int y = list_y + row * row_h;

	bg_restore(list_x, y, list_w, row_h);
	if (game_backdrop_active)
		blend_rect(list_x, y, list_w, row_h, 0x000000, 158);
	if (idx == sel)
	{
		if (game_backdrop_active) blend_rect(list_x, y, list_w, row_h, st.sel_bg, 210);
		else fill_rect(list_x, y, list_w, row_h, st.sel_bg);
	}

	if (idx >= item_cnt) return;

	entry_t *e = &items[idx];
	uint32_t fg = (idx == sel) ? st.sel_text : (e->is_dir ? st.accent : st.text);

	int px = st.font_px;
	int pad = skin()->row_pad;
	int tx = list_x + pad;
	int ty = y + (row_h - px) / 2;

	char label[NAME_LEN + 8];
	if (e->is_dir && ui_lowres) snprintf(label, sizeof(label), "> %s", e->disp[0] ? e->disp : e->name);
	else if (e->is_dir) snprintf(label, sizeof(label), "[DIR] %s", e->disp[0] ? e->disp : e->name);
	else snprintf(label, sizeof(label), "%s", e->disp[0] ? e->disp : e->name);

	ui_text(tx, ty, label, fg, px, list_w - 2 * pad, st.list_align);
}

static void render_scrollbar()
{
	int sb_x = list_x + list_w + 4 * ui_scale;
	int sb_h = list_rows * row_h;
	if (sb_x + 3 * ui_scale > scr_w) return;
	bg_restore(sb_x, list_y, 3 * ui_scale, sb_h);
	if (item_cnt > list_rows)
	{
		int th = sb_h * list_rows / item_cnt;
		if (th < 8 * ui_scale) th = 8 * ui_scale;
		int to = (sb_h - th) * scroll_top / (item_cnt - list_rows);
		fill_rect(sb_x, list_y + to, 3 * ui_scale, th, st.sel_bg);
	}
}

static void render_list()
{
	if (st.showcase) return;
	for (int r = 0; r < list_rows; r++) render_row(r);
	render_scrollbar();
	if (!item_cnt)
	{
		ui_text(list_x + skin()->row_pad, list_y + row_h, "（空目录）", st.dim,
			st.font_px, list_w, SKIN_ALIGN_LEFT);
	}
}

// Atlas/Carbon "system" view: full-bleed background + centered logo (carousel substitute)
static int find_system_asset(const char *es, const char *fallback_name,
	char *out, size_t sz, int want_logo)
{
	// System art now comes from the skin package, pre-baked at the target
	// resolution: sysbg/<es>.png for the backdrop, systems/<es>.png for the
	// logo. Nothing is rasterised at runtime.
	const char *sub = want_logo ? "systems" : "sysbg";
	static const char *exts[] = { "png", "jpg" };

	// try ES short name first, then the lowercased MiSTer folder name
	const char *names[2];
	int nnames = 0;
	if (es && es[0]) names[nnames++] = es;
	if (fallback_name && fallback_name[0])
	{
		static char low[NAME_LEN];
		snprintf(low, sizeof(low), "%s", fallback_name);
		for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
		if (!nnames || strcmp(low, names[0])) names[nnames++] = low;
	}

	char rel[NAME_LEN + 32];
	for (int ni = 0; ni < nnames; ni++)
	{
		for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		{
			snprintf(rel, sizeof(rel), "%s/%s.%s", sub, names[ni], exts[i]);
			if (skin_asset_path(rel, out, sz)) return 1;
		}
	}
	out[0] = 0;
	return 0;
}

// Showcase art cache. A fully populated slot is about 3.2 MiB
// (960x540 background + 800x400 logo). 64 slots use at most ~205 MiB,
// deliberately exploiting the HPS's 1 GiB DDR3 while leaving ample room for
// Linux, MiSTer, framebuffers and ffmpeg. Assets are decoded one per idle tick.
#define SC_CACHE_SLOTS 64
#define SC_BG_MAX_W    960
#define SC_BG_MAX_H    540
#define SC_LOGO_MAX_W  800
#define SC_LOGO_MAX_H  400
#define SC_BG_TINT     0xFFA0A0A0

struct sc_slot_t
{
	char key[48];
	uint32_t *bg;
	int bg_w, bg_h;
	uint32_t *logo;
	int logo_w, logo_h;
	int tried_bg;
	int tried_logo;
	uint32_t tick;
};

static sc_slot_t sc_cache[SC_CACHE_SLOTS];
static uint32_t sc_tick = 1;
static int sc_prefetch_idx = -1;
static int sc_prefetch_remaining = 0;
static size_t sc_cache_bytes = 0;
static int sc_slot_limit = 32;

static size_t sc_mem_available(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return 0;
	char line[160];
	unsigned long kb = 0;
	while (fgets(line, sizeof(line), f))
	{
		if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) break;
	}
	fclose(f);
	return (size_t)kb * 1024;
}

// ---------------------------------------------------------------------------
// Shared art cache budget
// ---------------------------------------------------------------------------
// The showcase art and the game thumbnails must not each size themselves from
// MemAvailable independently: whichever ran first would take the lot and
// starve the other. Both draw from this single pool and report their usage
// here, so the split stays predictable regardless of navigation order.

static size_t art_pool_limit = 0;
static size_t art_pool_used = 0;

// Thumbnails are the hot path (they change on every cursor move), so they get
// the larger share; showcase art is only touched at the systems level.
#define ART_SHARE_THUMB_PCT 60
#define ART_SHARE_SC_PCT    40

static void art_pool_init(void)
{
	// Size from what Linux can really use, not the board's nominal 1 GiB:
	// MiSTer commonly exposes only about 492 MiB to the HPS kernel. Add our
	// own usage back so re-entering a level does not shrink the limit and
	// strand slots that are already populated.
	size_t avail = sc_mem_available() + art_pool_used;
	const size_t keep = 256U * 1024 * 1024;
	size_t budget = (avail > keep) ? (avail - keep) : (48U * 1024 * 1024);

	size_t cap = (size_t)skin()->cache_mb * 1024 * 1024;
	if (budget > cap) budget = cap;
	art_pool_limit = budget;

	printf("FBUI: art cache pool %.1f MiB (MemAvailable %.1f MiB, in use %.1f MiB)\n",
		art_pool_limit / 1048576.0, sc_mem_available() / 1048576.0,
		art_pool_used / 1048576.0);
}

static size_t art_share(int pct)
{
	if (!art_pool_limit) art_pool_init();
	return art_pool_limit / 100 * pct;
}

static void sc_slot_clear(sc_slot_t *s)
{
	size_t n = 0;
	if (s->bg) n += (size_t)s->bg_w * s->bg_h * 4;
	if (s->logo) n += (size_t)s->logo_w * s->logo_h * 4;
	sc_cache_bytes -= n;
	art_pool_used -= n;
	free(s->bg);
	free(s->logo);
	memset(s, 0, sizeof(*s));
}

static void sc_cache_clear_all(void)
{
	for (int i = 0; i < SC_CACHE_SLOTS; i++) sc_slot_clear(&sc_cache[i]);
	sc_prefetch_idx = -1;
	sc_prefetch_remaining = 0;
}

static void sc_prefetch_reset(void)
{
	if (ui_level != LV_SYSTEMS || !st.showcase || item_cnt < 2)
	{
		sc_prefetch_idx = -1;
		sc_prefetch_remaining = 0;
		return;
	}
	// Slot count comes out of the shared pool, so the thumbnail cache keeps its
	// share no matter which level the user visits first.
	const skin_t *sk = skin();
	const size_t slot_bytes = (size_t)sk->sysbg_w * sk->sysbg_h * 4
		+ (size_t)sk->syslogo_w * sk->syslogo_h * 4;
	sc_slot_limit = (int)(art_share(ART_SHARE_SC_PCT) / (slot_bytes ? slot_bytes : 1));
	if (sc_slot_limit < 5) sc_slot_limit = 5;
	if (sc_slot_limit > SC_CACHE_SLOTS) sc_slot_limit = SC_CACHE_SLOTS;
	sc_prefetch_idx = sel + 1;
	if (sc_prefetch_idx >= item_cnt) sc_prefetch_idx = 0;
	sc_prefetch_remaining = item_cnt - 1;
	if (sc_prefetch_remaining > sc_slot_limit - 1)
		sc_prefetch_remaining = sc_slot_limit - 1;
	printf("FBUI: system art cache %d slots x %.1f MiB\n",
		sc_slot_limit, slot_bytes / 1048576.0);
}

static sc_slot_t *sc_find(const char *key)
{
	if (!key || !key[0]) return NULL;
	for (int i = 0; i < sc_slot_limit; i++)
		if (sc_cache[i].key[0] && !strcasecmp(sc_cache[i].key, key))
			return &sc_cache[i];
	return NULL;
}

static sc_slot_t *sc_alloc(const char *key)
{
	sc_slot_t *s = sc_find(key);
	if (s) { s->tick = ++sc_tick; return s; }
	int best = 0;
	for (int i = 1; i < sc_slot_limit; i++)
		if (sc_cache[i].tick < sc_cache[best].tick) best = i;
	sc_slot_clear(&sc_cache[best]);
	snprintf(sc_cache[best].key, sizeof(sc_cache[best].key), "%s", key);
	sc_cache[best].tick = ++sc_tick;
	return &sc_cache[best];
}

static void blit_rgb_scale(uint32_t *dst, int dw, int dh, const uint32_t *src, int sw, int sh)
{
	if (!dst || !src || sw < 1 || sh < 1) return;
	for (int y = 0; y < dh; y++)
	{
		int sy = y * sh / dh;
		uint32_t *drow = dst + (size_t)y * dw;
		const uint32_t *srow = src + (size_t)sy * sw;
		// mask off alpha: the source is ARGB, the shadow buffer is xRGB
		for (int x = 0; x < dw; x++) drow[x] = srow[x * sw / dw] & 0xFFFFFF;
	}
}

static void blit_logo_box(uint32_t *dst, int dst_w, int dst_h,
	int lx, int ly, int lw, int lh, const uint32_t *src, int sw, int sh)
{
	if (!src || sw < 1 || sh < 1) return;
	int dw = lw, dh = lh;
	if ((int64_t)sw * lh > (int64_t)sh * lw) dh = sh * lw / sw;
	else dw = sw * lh / sh;
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	int ox = lx + (lw - dw) / 2;
	int oy = ly + (lh - dh) / 2;
	for (int y = 0; y < dh; y++)
	{
		int sy = y * sh / dh;
		for (int x = 0; x < dw; x++)
		{
			int sx = x * sw / dw;
			uint32_t c = src[sy * sw + sx];
			uint32_t a = c >> 24;
			if (!a) continue;
			int fx = ox + x, fy = oy + y;
			if ((unsigned)fx >= (unsigned)dst_w || (unsigned)fy >= (unsigned)dst_h) continue;
			if (a >= 250) dst[fy * dst_w + fx] = c & 0xFFFFFF;
			else
			{
				uint32_t d = dst[fy * dst_w + fx];
				uint32_t r = ((c >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a);
				uint32_t g = ((c >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a);
				uint32_t b = (c & 255) * a + (d & 255) * (255 - a);
				dst[fy * dst_w + fx] = ((r / 255) << 16) | ((g / 255) << 8) | (b / 255);
			}
		}
	}
}

// Decode at most one asset per call so a single UI tick cannot stall for
// both a JPEG background and an SVG logo.
static int sc_decode_one(sc_slot_t *s, const char *es, const char *fallback_name)
{
	if (!s) return 0;
	char path[1300];
	const skin_t *sk = skin();
	if (!s->tried_bg)
	{
		s->tried_bg = 1;
		if (find_system_asset(es, fallback_name, path, sizeof(path), 0))
		{
			s->bg = skin_img_load_cover(path, sk->sysbg_w, sk->sysbg_h, SC_BG_TINT,
				&s->bg_w, &s->bg_h);
			if (s->bg)
			{
				size_t n = (size_t)s->bg_w * s->bg_h * 4;
				sc_cache_bytes += n;
				art_pool_used += n;
			}
		}
		return 1;
	}
	if (!s->tried_logo)
	{
		s->tried_logo = 1;
		if (find_system_asset(es, fallback_name, path, sizeof(path), 1))
		{
			s->logo = skin_img_load_fit(path, sk->syslogo_w, sk->syslogo_h,
				&s->logo_w, &s->logo_h);
			if (s->logo)
			{
				size_t n = (size_t)s->logo_w * s->logo_h * 4;
				sc_cache_bytes += n;
				art_pool_used += n;
			}
		}
		return 1;
	}
	return 0;
}

static int sc_ensure_index(int idx)
{
	if (idx < 0 || idx >= item_cnt) return 0;
	const char *es = es_sys_name(items[idx].name);
	sc_slot_t *s = sc_alloc(es && es[0] ? es : items[idx].name);
	return sc_decode_one(s, es, items[idx].name);
}

static int sc_prefetch_step(void)
{
	while (sc_prefetch_remaining > 0 && item_cnt > 1)
	{
		if (sc_prefetch_idx < 0 || sc_prefetch_idx >= item_cnt)
			sc_prefetch_idx = 0;
		if (sc_prefetch_idx == sel)
		{
			sc_prefetch_idx++;
			continue;
		}
		if (sc_ensure_index(sc_prefetch_idx)) return 1;
		sc_prefetch_idx++;
		if (sc_prefetch_idx >= item_cnt) sc_prefetch_idx = 0;
		sc_prefetch_remaining--;
	}
	if (!sc_prefetch_remaining && sc_prefetch_idx >= 0)
	{
		printf("FBUI: system art cache ready: %.1f MiB, up to %d systems\n",
			sc_cache_bytes / 1048576.0, sc_slot_limit);
		sc_prefetch_idx = -1;
	}
	return 0;
}

static void showcase_geom(int *lx, int *ly, int *lw, int *lh)
{
	int sw = scr_w - safe_l - safe_r;
	int sh = scr_h - safe_t - safe_b;
	*lx = safe_l + sw * 10 / 100;
	*ly = safe_t + sh * 18 / 100;
	*lw = sw * 80 / 100;
	*lh = sh * 40 / 100;
}

static void render_showcase_labels(entry_t *e, int have_logo, int lx, int ly, int lw, int lh)
{
	// Always show the system title. Logos alone are often icon-only / hard to
	// read, so hiding text when a logo exists left some systems untitled.
	const char *label = e->disp[0] ? e->disp : e->name;
	// Leave a real gap above the 240p footer. The CRT-safe layout has very
	// little vertical slack, so lift the title+counter group by eight pixels.
	int ny = ly + lh + (ui_lowres ? -6 : scr_h * 2 / 100);
	if (!have_logo)
	{
		int px = ui_lowres ? 16 : st.font_px * 2;
		ui_text(lx, ly + lh / 2 - px / 2, label, 0xFFFFFF, px, lw, SKIN_ALIGN_CENTER);
	}
	int npx = ui_lowres ? 16 : st.font_px + st.font_px / 4;
	ui_text(lx, ny, label, 0xF0F0F0, npx, lw, SKIN_ALIGN_CENTER);
	ny += npx + (ui_lowres ? 2 : st.font_px / 2);
	char cnt[48];
	snprintf(cnt, sizeof(cnt), "%d / %d", sel + 1, item_cnt);
	int cpx = ui_lowres ? 16 : st.font_px;
	ui_text(scr_w / 4, ny, cnt, 0xC0C0C0, cpx, scr_w / 2, SKIN_ALIGN_CENTER);
}

// Lightweight scrub while the stick is moving: only refresh the title strip.
// Avoids a full 1080p dirty flush on every tick (that was the main stall).
static void render_showcase_scrub(void)
{
	// At 240p the visible background is selection-specific, while bglayer is
	// only the static theme. Restoring a narrow title strip from bglayer creates
	// a conspicuous black flash across the counter. Keep the current complete
	// frame during navigation and repaint once after the 140ms settle timer.
	if (ui_lowres) return;
	if (!item_cnt || sel >= item_cnt) return;
	entry_t *e = &items[sel];
	int lx, ly, lw, lh;
	showcase_geom(&lx, &ly, &lw, &lh);
	int npx = ui_lowres ? 16 : st.font_px + st.font_px / 4;
	int ty = ly + lh + (ui_lowres ? 2 : scr_h * 2 / 100);
	int cpx = ui_lowres ? 16 : st.font_px;
	int gap = ui_lowres ? 2 : st.font_px / 2;
	int th = npx + gap + cpx + (ui_lowres ? 2 : scr_h / 50);
	if (ty + th > scr_h) th = scr_h - ty;
	if (th < 1) return;

	bg_restore(0, ty, scr_w, th);
	const char *label = e->disp[0] ? e->disp : e->name;
	ui_text(lx, ty, label, 0xF0F0F0, npx, lw, SKIN_ALIGN_CENTER);
	char cnt[48];
	snprintf(cnt, sizeof(cnt), "%d / %d", sel + 1, item_cnt);
	ui_text(scr_w / 4, ty + npx + gap, cnt, 0xC0C0C0, cpx, scr_w / 2, SKIN_ALIGN_CENTER);
}

// Paint from cache only (no decode). Used on settle when assets already warm.
static void render_showcase_paint(void)
{
	if (!item_cnt || sel >= item_cnt) return;
	entry_t *e = &items[sel];
	const char *es = es_sys_name(e->name);
	const char *key = (es && es[0]) ? es : e->name;
	bg_restore(0, 0, scr_w, scr_h);
	int lx, ly, lw, lh;
	showcase_geom(&lx, &ly, &lw, &lh);
	int have_logo = 0;
	sc_slot_t *s = sc_find(key);
	if (s && s->bg)
	{
		blit_rgb_scale(shadow, scr_w, scr_h, s->bg, s->bg_w, s->bg_h);
	}
	if (s && s->logo)
	{
		have_logo = 1;
		blit_logo_box(shadow, scr_w, scr_h, lx, ly, lw, lh, s->logo, s->logo_w, s->logo_h);
	}
	render_showcase_labels(e, have_logo, lx, ly, lw, lh);
}

static void render_showcase(void)
{
	if (!item_cnt || sel >= item_cnt) return;
	entry_t *e = &items[sel];
	const char *es = es_sys_name(e->name);
	const char *key = (es && es[0]) ? es : e->name;
	sc_slot_t *s = sc_alloc(key);
	// At most one decode here; remaining assets finish on idle ticks.
	sc_decode_one(s, es, e->name);

	render_showcase_paint();

}

// ---------------------------------------------------------------------------
// Game thumbnail cache
// ---------------------------------------------------------------------------
// Entries are decoded straight to the panel box (pixel-aspect corrected), so a
// cache hit is a plain alpha blend with no scaling. The window is sized by
// BYTES, not by page count: at 240p a slot is ~130 KiB and the budget holds
// hundreds of entries — effectively the whole list — while at 1080p a slot is
// ~2.8 MiB and only the visible page plus a read-ahead margin fits. Sizing by
// "current page plus one either side" would quietly need 194 MiB at 1080p and
// collide with the showcase cache.

#define TH_MAX_SLOTS 512

struct th_slot_t
{
	char key[2400];   // source path
	uint32_t *px;     // decoded ARGB at the panel box size (NULL = decode failed)
	int w, h;
	uint32_t tick;
};

static th_slot_t *th_cache = NULL;
static int th_slots = 0;
static uint32_t th_tick = 1;
static size_t th_bytes = 0;
static int th_prefetch_i = 0;

static void th_slot_clear(th_slot_t *t)
{
	if (t->px)
	{
		size_t n = (size_t)t->w * t->h * 4;
		th_bytes -= n;
		art_pool_used -= n;
		free(t->px);
	}
	memset(t, 0, sizeof(*t));
}

static void th_cache_clear_all(void)
{
	for (int i = 0; i < th_slots; i++) th_slot_clear(&th_cache[i]);
	th_prefetch_i = 0;
}

static void th_cache_free(void)
{
	th_cache_clear_all();
	free(th_cache);
	th_cache = NULL;
	th_slots = 0;
}

// Derive the window from the shared pool and the skin's fixed slot size.
static void th_cache_reset(void)
{
	const skin_t *s = skin();
	size_t slot = (size_t)s->thumb_w * s->thumb_h * 4;
	if (!slot) slot = 1;

	int n = (int)(art_share(ART_SHARE_THUMB_PCT) / slot);
	int floor_n = list_rows + 4;   // visible page plus read-ahead
	if (n < floor_n) n = floor_n;
	if (n > TH_MAX_SLOTS) n = TH_MAX_SLOTS;

	if (n != th_slots)
	{
		th_cache_free();
		th_cache = (th_slot_t*)calloc((size_t)n, sizeof(th_slot_t));
		th_slots = th_cache ? n : 0;
	}
	th_prefetch_i = 0;
	printf("FBUI: thumbnail cache %d slots x %dx%d (%.1f MiB max)\n",
		th_slots, s->thumb_w, s->thumb_h, (th_slots * slot) / 1048576.0);
}

static th_slot_t *th_find(const char *key)
{
	for (int i = 0; i < th_slots; i++)
		if (th_cache[i].key[0] && !strcmp(th_cache[i].key, key)) return &th_cache[i];
	return NULL;
}

// Fetch (decoding on miss). A failed decode still occupies its slot with a
// NULL bitmap so the same unreadable file is not retried on every repaint.
static th_slot_t *th_get(const char *path, int bw, int bh)
{
	if (!path || !path[0] || th_slots < 1 || !th_cache) return NULL;

	th_slot_t *t = th_find(path);
	if (t) { t->tick = ++th_tick; return t; }

	int best = 0;
	for (int i = 1; i < th_slots; i++)
		if (th_cache[i].tick < th_cache[best].tick) best = i;
	t = &th_cache[best];
	th_slot_clear(t);
	snprintf(t->key, sizeof(t->key), "%s", path);
	t->tick = ++th_tick;

	// Honour the display pixel aspect here, once: a 240p entry is stored
	// pre-squashed so it can be blitted 1:1 later.
	t->px = skin_img_load_fit_par(path, bw, bh, &t->w, &t->h);
	if (t->px)
	{
		size_t n = (size_t)t->w * t->h * 4;
		th_bytes += n;
		art_pool_used += n;
	}
	return t;
}

static int thumb_draw(const char *path, int bx, int by, int bw, int bh)
{
	th_slot_t *t = th_get(path, bw, bh);
	if (!t || !t->px) return 0;

	int ox, oy, ow, oh;
	skin_img_blend_box(shadow, scr_w, scr_h, t->px, t->w, t->h, bx, by, bw, bh,
		&ox, &oy, &ow, &oh);
	if (ow > 0 && oh > 0) dirty_add(ox, oy, ox + ow, oy + oh);
	return 1;
}

// Warm one neighbour per idle tick, expanding outward from the selection so
// the visible page fills first. The counter is monotonic, so a fully warm
// list costs nothing.
static int th_offset_at(int i)
{
	if (!i) return 0;
	int d = (i + 1) / 2;
	return (i & 1) ? d : -d;
}

static int th_prefetch_step(void)
{
	if (ui_level != LV_GAMES || !item_cnt || th_slots < 1) return 0;

	while (th_prefetch_i < th_slots)
	{
		int idx = sel + th_offset_at(th_prefetch_i++);
		if (idx < 0 || idx >= item_cnt || items[idx].is_dir) continue;

		char cover[2400] = "";
		find_media(items[idx].name, cover, sizeof(cover), NULL, 0);
		if (!cover[0] || th_find(cover)) continue;

		th_get(cover, panel_w, panel_h);
		return 1;
	}
	return 0;
}

static void render_panel()
{
	if (st.showcase) return;
	if (game_backdrop_active)
	{
		preview_stop();
		return;
	}

	int bx = panel_x, by = panel_y, bw = panel_w, bh = panel_h;
	bg_restore(bx, by, bw, bh);
	if (!item_cnt || sel >= item_cnt) return;
	entry_t *e = &items[sel];

	if (ui_level == LV_GAMES && !e->is_dir)
	{
		char cover[2400] = "", video[2400] = "";
		find_media(e->name, cover, sizeof(cover), video, sizeof(video));
		if (video[0])
		{
			preview_start(video, bx, by, bw, bh);
			if (!preview_blit(bx, by, bw, bh) && cover[0]) thumb_draw(cover, bx, by, bw, bh);
		}
		else
		{
			preview_stop();
			if (cover[0]) thumb_draw(cover, bx, by, bw, bh);
			// no media -> leave the static layer showing
		}
		return;
	}

	// systems / directories: the skin's pre-baked system logo when it has one
	if (ui_level == LV_SYSTEMS)
	{
		char p[1300];
		if (find_system_asset(es_sys_name(e->name), e->name, p, sizeof(p), 1)
		    && thumb_draw(p, bx, by, bw, bh))
			return;
	}

	// no art: a large dim label instead
	ui_text(bx, by + bh / 2 - st.font_px, e->disp[0] ? e->disp : e->name, st.dim,
		st.font_px * 2, bw, SKIN_ALIGN_CENTER);
}

static void render_full()
{
	render_bglayer();
	game_backdrop_active = render_game_backdrop_layer();
	bg_restore(0, 0, scr_w, scr_h);
	if (st.showcase)
	{
		render_showcase();
		render_footer();
		return;
	}
	render_header();
	render_list();
	render_panel();
	render_footer();
}

static void show_msg(const char *fmt, const char *arg)
{
	snprintf(msg_text, sizeof(msg_text), fmt, arg);
	msg_timer = GetTimer(3000);
	render_footer();
}

// ---------------------------------------------------------------------------
// Launch via MGL
// ---------------------------------------------------------------------------

static void xml_escape(const char *in, char *out, size_t sz)
{
	size_t o = 0;
	for (const char *p = in; *p && o + 8 < sz; p++)
	{
		switch (*p)
		{
		case '&':  o += snprintf(out + o, sz - o, "&amp;");  break;
		case '<':  o += snprintf(out + o, sz - o, "&lt;");   break;
		case '>':  o += snprintf(out + o, sz - o, "&gt;");   break;
		case '"':  o += snprintf(out + o, sz - o, "&quot;"); break;
		default: out[o++] = *p;
		}
	}
	out[o] = 0;
}

// ---------------------------------------------------------------------------
// Boot resolution picker (ugly hardware OSD) — before first FBUI start
// ---------------------------------------------------------------------------

#define BOOTRES_TIMEOUT_MS 8000

static int bootres_done = 0;       // finished for this boot
static int bootres_choice = 0;     // 0=240p, 1=1080p
static int bootres_sel = 0;        // highlight
static int bootres_active = 0;     // picker on screen
static unsigned long bootres_deadline = 0;
static unsigned long bootres_redraw_at = 0;

static void bootres_draw(void)
{
	int left_ms = 0;
	if (bootres_deadline)
	{
		// GetTimer(0) is "now"; deadline is absolute. Approximate remaining.
		unsigned long now = GetTimer(0);
		if (bootres_deadline > now) left_ms = (int)(bootres_deadline - now);
	}
	int left_s = (left_ms + 999) / 1000;
	if (left_s < 0) left_s = 0;

	OsdSetSize(16);
	OsdSetTitle("Video Output", 0);
	int n = 0;
	OsdWrite(n++);
	OsdWrite(n++, "  Select resolution");
	OsdWrite(n++, "  (HDMI and VGA same)");
	OsdWrite(n++);
	OsdWrite(n++, bootres_sel == 0 ? " > 240p  CRT / 15kHz" : "   240p  CRT / 15kHz",
		bootres_sel == 0);
	OsdWrite(n++, bootres_sel == 1 ? " > 1080p HDMI" : "   1080p HDMI",
		bootres_sel == 1);
	OsdWrite(n++);
	char line[40];
	snprintf(line, sizeof(line), "  Auto 240p in %ds", left_s);
	OsdWrite(n++, line);
	OsdWrite(n++);
	OsdWrite(n++, "  Up/Down   Enter=OK");
	for (; n < OsdGetSize(); n++) OsdWrite(n);
	OsdUpdate();
	OsdEnable(DISABLE_KEYBOARD);
	OsdMenuCtl(1);
}

// Returns 1 when picker finished (choice applied); 0 while still showing.
static int bootres_tick(uint32_t c)
{
	if (bootres_done) return 1;

	if (!bootres_active)
	{
		bootres_active = 1;
		bootres_sel = 0; // default 240p
		bootres_deadline = GetTimer(BOOTRES_TIMEOUT_MS);
		bootres_redraw_at = GetTimer(200);
		user_io_osd_key_enable(1);
		bootres_draw();
		printf("FBUI: boot resolution picker\n");
		return 0;
	}

	int confirm = 0;
	if (c && !(c & UPSTROKE))
	{
		uint32_t k = c & ~UPSTROKE;
		if (k == KEY_UP || k == KEY_LEFT || k == KEY_PAGEUP)
		{
			bootres_sel = 0;
			bootres_deadline = GetTimer(BOOTRES_TIMEOUT_MS);
			bootres_draw();
		}
		else if (k == KEY_DOWN || k == KEY_RIGHT || k == KEY_PAGEDOWN)
		{
			bootres_sel = 1;
			bootres_deadline = GetTimer(BOOTRES_TIMEOUT_MS);
			bootres_draw();
		}
		else if (k == KEY_ENTER || k == KEY_SPACE)
		{
			confirm = 1;
		}
	}

	if (!confirm && CheckTimer(bootres_deadline))
	{
		bootres_sel = 0; // timeout -> 240p
		confirm = 1;
		printf("FBUI: bootres timeout -> 240p\n");
	}

	if (confirm)
	{
		bootres_choice = bootres_sel;
		bootres_done = 1;
		bootres_active = 0;
		video_menu_res_apply(bootres_choice);
		OsdDisable();
		printf("FBUI: bootres selected %s\n", bootres_choice ? "1080p" : "240p");
		return 1;
	}

	if (CheckTimer(bootres_redraw_at))
	{
		bootres_redraw_at = GetTimer(200);
		bootres_draw();
	}
	return 0;
}

static void fbui_stop(); // fwd

static void launch_game(entry_t *e)
{
	if (!cur_map)
	{
		show_msg("未配置核心映射: %s（请在 fbui.cpp 的 sys_maps 表中添加）", cur_sys);
		return;
	}

	preview_stop();

	// Arcade MRA: one XML description per game (CPS1/2/… cores resolved inside)
	if (cur_map->kind == SYS_KIND_ARCADE)
	{
		char full[2048];
		snprintf(full, sizeof(full), "%s/_Arcade/%s", getRootDir(), e->name);
		printf("FBUI: launching MRA %s\n", full);
		fbui_stop();
		xml_load(full);
		return;
	}

	char dir[2048], full[2400], esc[3000];
	cur_dir_path(dir, sizeof(dir));
	snprintf(full, sizeof(full), "%s/%s", dir, e->name);
	xml_escape(full, esc, sizeof(esc));

	FILE *f = fopen("/tmp/fbui.mgl", "w");
	if (!f)
	{
		show_msg("无法写入 /tmp/fbui.mgl%s", "");
		return;
	}
	fprintf(f,
		"<mistergamedescription>\n"
		"\t<rbf>%s</rbf>\n"
		"\t<file delay=\"%d\" type=\"%c\" index=\"%d\" path=\"%s\"/>\n"
		"</mistergamedescription>\n",
		cur_map->rbf, cur_map->delay, cur_map->type, cur_map->index, esc);
	fclose(f);

	printf("FBUI: launching %s via %s\n", full, cur_map->rbf);
	fbui_stop();
	xml_load("/tmp/fbui.mgl");
}

// ---------------------------------------------------------------------------
// Start / stop / input
// ---------------------------------------------------------------------------

int fbui_active()
{
	return ui_active;
}

static int fbui_start()
{
	int w, h;
	uint32_t *plane = video_fb_get_plane(1, &w, &h);
	if (!plane || w <= 0 || h <= 0) return 0;

	if (!items) items = (entry_t*)malloc(sizeof(entry_t) * FBUI_MAX_ITEMS);
	if (!items) return 0;

	if (!shadow || w != scr_w || h != scr_h)
	{
		free(shadow);
		free(bglayer);
		shadow = (uint32_t*)malloc((size_t)w * h * 4);
		bglayer = (uint32_t*)malloc((size_t)w * h * 4);
		if (!shadow || !bglayer)
		{
			free(shadow);
			free(bglayer);
			shadow = NULL;
			bglayer = NULL;
			return 0;
		}
	}
	scr_w = w;
	scr_h = h;

	hexfont_load();
	// The boot picker fixes the output resolution for the whole session, so the
	// skin package is resolved and parsed exactly once here.
	skin_load(cfg.fbui_skin, scr_w, scr_h);
	skin_apply();
	layout_compute();
	art_pool_init();
	dirty_reset();

	enter_systems();
	render_full();

	OsdMenuCtl(0);            // hide the hardware OSD overlay
	user_io_osd_key_enable(1); // route keyboard/gamepad to the menu key queue
	flush_to_plane();
	video_fb_enable(1, 1);  // show plane 1 (wallpaper-style: input stays with MiSTer)

	ui_active = 1;
	msg_text[0] = 0;
	printf("FBUI: started (%dx%d, scale %d)\n", scr_w, scr_h, ui_scale);
	return 1;
}

static void fbui_stop()
{
	if (!ui_active) return;
	ui_active = 0;
	preview_stop();
	sc_cache_clear_all();
	th_cache_free();
	fbui_ttf_cache_clear();
	skin_unload();

	// restore the normal wallpaper + OSD menu
	video_menu_bg(user_io_status_get("[3:1]"));
	OsdMenuCtl(1);
	// leave unified menu video; restore INI timings for classic OSD / games
	video_menu_res_restore();
	printf("FBUI: stopped\n");
}

static int is_nav_key(uint32_t c)
{
	c &= ~UPSTROKE;
	return c == KEY_UP || c == KEY_DOWN || c == KEY_LEFT || c == KEY_RIGHT
		|| c == KEY_PAGEUP || c == KEY_PAGEDOWN || c == KEY_HOME || c == KEY_END;
}

static void move_sel(int delta)
{
	if (!item_cnt) return;
	int old = sel;
	sel += delta;
	if (sel < 0) sel = 0;
	if (sel >= item_cnt) sel = item_cnt - 1;
	if (sel == old) return;

	// Showcase: only refresh the title strip while moving. Full art paints
	// after input settles — avoids stalling the input loop on 1080p copies
	// and SVG decode (which caused "no response then jump many entries").
	if (st.showcase)
	{
		showcase_hold = GetTimer(140);
		render_showcase_scrub();
		return;
	}

	int old_top = scroll_top;
	if (sel < scroll_top) scroll_top = sel;
	if (sel >= scroll_top + list_rows) scroll_top = sel - list_rows + 1;

	if (scroll_top != old_top)
	{
		need_list_redraw = 1;
	}
	else
	{
		render_row(old - scroll_top);
		render_row(sel - scroll_top);
		render_scrollbar();
	}
	if (ui_level == LV_GAMES)
	{
		// Read-ahead is centred on the selection, so restart the walk whenever
		// it moves. Already-cached neighbours are skipped, so this is cheap.
		th_prefetch_i = 0;
		// The selected screenshot is part of bglayer, so changing selection
		// requires one complete composition rather than a right-panel repaint.
		need_full_redraw = 1;
		need_list_redraw = 0;
		need_panel_redraw = 0;
		return;
	}
	need_panel_redraw = 1;
	render_header(); // update counter
}

static void apply_nav_key(uint32_t c)
{
	switch (c & ~UPSTROKE)
	{
	case KEY_UP:        move_sel(-1); break;
	case KEY_DOWN:      move_sel(1); break;
	case KEY_PAGEUP:    move_sel(st.showcase ? -1 : -list_rows); break;
	case KEY_LEFT:      move_sel(st.showcase ? -1 : -list_rows); break;
	case KEY_PAGEDOWN:  move_sel(st.showcase ? 1 : list_rows); break;
	case KEY_RIGHT:     move_sel(st.showcase ? 1 : list_rows); break;
	case KEY_HOME:      move_sel(-item_cnt); break;
	case KEY_END:       move_sel(item_cnt); break;
	}
}

static void handle_key(uint32_t c)
{
	// After a heavy decode, ignore held/repeat nav until the key is released.
	// Otherwise queued time makes CheckTimer fire and we jump several slots.
	if (nav_busy)
	{
		if ((c & UPSTROKE) || !c)
		{
			nav_busy = 0;
			nav_held = 0;
		}
		else if (is_nav_key(c))
			return;
	}

	if (c & UPSTROKE)
	{
		if (is_nav_key(c)) nav_held = 0;
	}
	else if (is_nav_key(c))
	{
		nav_held = c;
		nav_repeat_at = GetTimer(380);
		apply_nav_key(c);
		return;
	}

	switch (c)
	{
	case KEY_ENTER:
	case KEY_KPENTER:
	case KEY_SPACE:
		if (!item_cnt) break;
		if (ui_level == LV_SYSTEMS)
		{
			snprintf(cur_sys, sizeof(cur_sys), "%s", items[sel].name);
			cur_rel[0] = 0;
			cur_map = find_sys_map(cur_sys);
			enter_games();
		}
		else if (items[sel].is_dir)
		{
			if (cur_rel[0]) snprintf(cur_rel + strlen(cur_rel), sizeof(cur_rel) - strlen(cur_rel), "/%s", items[sel].name);
			else snprintf(cur_rel, sizeof(cur_rel), "%s", items[sel].name);
			enter_games();
		}
		else
		{
			launch_game(&items[sel]);
		}
		break;

	case KEY_BACKSPACE | UPSTROKE:
	case KEY_BACK | UPSTROKE:
		if (ui_level == LV_GAMES)
		{
			if (cur_rel[0])
			{
				char *slash = strrchr(cur_rel, '/');
				if (slash) *slash = 0;
				else cur_rel[0] = 0;
				enter_games();
			}
			else
			{
				enter_systems();
			}
		}
		break;
	}
}

int fbui_ui_hook(uint32_t *pc)
{
	uint32_t c = *pc;

	if (!ui_active)
	{
		// Boot resolution picker (OSD) before first FBUI start
		if (cfg.fbui && !bootres_done)
		{
			if (!bootres_tick(c))
			{
				*pc = 0;
				return 1;
			}
		}

		// autostart once when enabled in INI; F3 (re)enters any time
		static int autostart_done = 0;
		int want = 0;

		if (cfg.fbui && !autostart_done) want = 1;
		if (c == KEY_F3) want = 1;

		if (want && !video_fb_state())
		{
			// Re-apply chosen unified mode (e.g. after ESC restored INI)
			if (bootres_done) video_menu_res_apply(bootres_choice);
			if (fbui_start())
			{
				autostart_done = 1;
				return 1;
			}
		}
		return 0;
	}

	// exit keys -> back to classic OSD menu
	if (c == (KEY_ESC | UPSTROKE) || c == KEY_F12 || c == (KEY_F12 | UPSTROKE))
	{
		if (c == KEY_F12) return 1; // act on release only
		fbui_stop();
		return 2;
	}

	if (c) handle_key(c);
	else if (nav_held && !nav_busy && CheckTimer(nav_repeat_at))
	{
		// menu_key_get does not auto-repeat arrows outside file-select;
		// generate a local repeat so holding D-pad feels continuous.
		nav_repeat_at = GetTimer(70);
		apply_nav_key(nav_held);
	}

	if (showcase_hold && CheckTimer(showcase_hold))
	{
		showcase_hold = 0;
		nav_busy = 1;
		render_showcase();
		render_footer();
		// Require a fresh press before more nav — prevents the "stuck then
		// jump several pages" burst after a slow decode.
		nav_held = 0;
		nav_busy = 0;
	}
	else if (!showcase_hold && !nav_held
	         && !need_full_redraw && !need_list_redraw && !need_panel_redraw)
	{
		if (st.showcase)
		{
			// idle: at most ONE decode per tick (current first, then neighbors)
			int did = 0;
			if (item_cnt && sel < item_cnt)
			{
				const char *es = es_sys_name(items[sel].name);
				const char *key = (es && es[0]) ? es : items[sel].name;
				sc_slot_t *cur = sc_alloc(key);
				if (sc_decode_one(cur, es, items[sel].name))
				{
					render_showcase_paint();
					render_footer();
					did = 1;
				}
			}
			if (!did) sc_prefetch_step();
		}
		else if (ui_level == LV_GAMES)
		{
			// Warm one neighbouring cover per tick so scrolling lands on
			// entries that are already decoded.
			th_prefetch_step();
		}
	}

	// keep video preview frames refreshing while ffmpeg updates the PNG
	if (prev_pid > 0) need_panel_redraw = 1;

	if (need_full_redraw)
	{
		need_full_redraw = 0;
		need_list_redraw = 0;
		need_panel_redraw = 0;
		render_full();
	}
	if (need_list_redraw)
	{
		need_list_redraw = 0;
		render_list();
	}
	if (need_panel_redraw)
	{
		need_panel_redraw = 0;
		render_panel();
	}
	if (msg_text[0] && CheckTimer(msg_timer)) render_footer();

	flush_to_plane();
	return 1;
}
