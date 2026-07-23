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
#include "fbui_theme.h"
#include "file_io.h"
#include "video.h"
#include "osd.h"
#include "cfg.h"
#include "input.h"
#include "user_io.h"
#include "charrom.h"
#include "font_cjk.h"
#include "support/arcade/mra_loader.h"
#include "lib/imlib2/Imlib2.h"
#include "sxmlc.h"

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

static void put_px(int x, int y, uint32_t color, int s)
{
	fill_rect(x, y, s, s, color);
}

// scale s: glyph cell is 16*s px tall. Returns x advance.
static int draw_glyph(int x, int y, uint32_t cp, uint32_t color, int s)
{
	const uint8_t *rec = fbui_hexfont_get(cp);
	if (rec)
	{
		int wide = rec[0];
		const uint8_t *bm = rec + 1;
		for (int row = 0; row < 16; row++)
		{
			if (wide)
			{
				uint16_t bits = ((uint16_t)bm[row * 2] << 8) | bm[row * 2 + 1];
				for (int col = 0; col < 16; col++)
					if (bits & (0x8000 >> col)) put_px(x + col * s, y + row * s, color, s);
			}
			else
			{
				uint8_t bits = bm[row];
				for (int col = 0; col < 8; col++)
					if (bits & (0x80 >> col)) put_px(x + col * s, y + row * s, color, s);
			}
		}
		return (wide ? 16 : 8) * s;
	}

	// built-in 12x12 CJK sample (column-major, bit0 = top), centered in 16px
	const uint16_t *cols = (cp >= 0x80) ? font_cjk_get(cp) : NULL;
	if (cols)
	{
		for (int col = 0; col < FONT_CJK_WIDTH; col++)
		{
			uint16_t bits = cols[col];
			for (int row = 0; row < FONT_CJK_HEIGHT; row++)
				if (bits & (1 << row)) put_px(x + (col + 2) * s, y + (row + 2) * s, color, s);
		}
		return 16 * s;
	}

	if (cp < 0x80)
	{
		// built-in OSD 8x8 font (column-major, bit0 = top), doubled to 8x16 cell
		const unsigned char *g = charfont[cp & 0x7F];
		for (int col = 0; col < 8; col++)
		{
			for (int row = 0; row < 8; row++)
				if (g[col] & (1 << row)) fill_rect(x + col * s, y + (row * 2 + 1) * s, s, 2 * s, color);
		}
		return 8 * s;
	}

	// missing glyph: hollow box
	int w = 14 * s;
	fill_rect(x + s, y + 2 * s, w, s, color);
	fill_rect(x + s, y + 13 * s, w, s, color);
	fill_rect(x + s, y + 2 * s, s, 12 * s, color);
	fill_rect(x + w, y + 2 * s, s, 12 * s, color);
	return 16 * s;
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
		int adv = (cp < 0x80 ? 8 : 16) * s;
		if (max_w && cx + adv > max_w)
		{
			// ellipsis
			if (max_w >= cx + 6 * s)
				for (int i = 0; i < 3; i++) fill_rect(x + cx + i * 2 * s + s, y + 14 * s, s, s, color);
			break;
		}
		cx += draw_glyph(x + cx, y, cp, color, s);
	}
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
		w += (cp < 0x80 ? 8 : 16) * s;
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
static pid_t prev_pid = -1;

// layout (computed from scr_w/h)
static int ui_scale;
static int ui_lowres;
static int header_h, footer_h, row_h, list_x, list_y, list_w, list_rows;
static int panel_x, panel_y, panel_w, panel_h;
// CRTs commonly overscan. Keep the complete 240p composition inside this inset.
static int safe_l, safe_t, safe_r, safe_b;

// colors
#define COL_BG_TOP    0x1A2029
#define COL_BG_BOT    0x0D1015
#define COL_HEADER    0x232B38
#define COL_TEXT      0xC8D0DA
#define COL_DIM       0x707A88
#define COL_SEL_BG    0x2A6BD8
#define COL_SEL_TEXT  0xFFFFFF
#define COL_ACCENT    0x4A90E2
#define COL_PANEL     0x161B24

// active style: defaults above, or values pulled from the loaded ES theme
static struct
{
	int use_theme;
	int showcase;     // system-view carousel style (one system, full-bleed art)
	uint32_t text, dim, sel_bg, sel_text, accent;
	int font_px;      // list row text height
	int list_align;   // THEME_ALIGN_*
	int have_panel;   // theme provides an md_image slot (or classic mode)
} st;

static void layout_compute()
{
	// Keep this decision local to the framebuffer. The video-mode state must not
	// participate in drawing: a mode switch and FB remap do not become visible
	// atomically, which can otherwise select geometry for the wrong buffer.
	ui_lowres = (scr_h <= 288);
	ui_scale = (scr_h >= 900) ? 2 : 1;
	safe_l = safe_r = ui_lowres ? (scr_w * 6 / 100) : 0;
	safe_t = safe_b = ui_lowres ? (scr_h * 6 / 100) : 0;

	if (ui_lowres)
	{
		// Dedicated 240p geometry: 32px CJK in 36px rows, but compact chrome.
		// At 640x240 this leaves three visible list rows plus a usable panel.
		header_h = 36;
		footer_h = 36;
		row_h = 36;
		list_x = safe_l + 8;
		list_y = safe_t + header_h + 4;
		list_w = (scr_w - safe_l - safe_r) * 58 / 100 - 8;
		list_rows = (scr_h - safe_b - footer_h - 4 - list_y) / row_h;
		if (list_rows < 1) list_rows = 1;
		panel_x = safe_l + (scr_w - safe_l - safe_r) * 60 / 100;
		panel_y = list_y;
		panel_w = scr_w - safe_r - panel_x - 8;
		panel_h = scr_h - safe_b - footer_h - 4 - panel_y;
		return;
	}

	header_h = 28 * ui_scale;
	footer_h = 24 * ui_scale;
	row_h = 20 * ui_scale;
	list_x = safe_l + 12 * ui_scale;
	list_y = safe_t + header_h + 8 * ui_scale;
	list_w = (scr_w - safe_l - safe_r) * 58 / 100 - 12 * ui_scale;
	list_rows = (scr_h - safe_b - list_y - footer_h - 8 * ui_scale) / row_h;
	if (list_rows < 1) list_rows = 1;
	panel_x = safe_l + (scr_w - safe_l - safe_r) * 60 / 100;
	panel_y = list_y;
	panel_w = scr_w - safe_r - panel_x - 12 * ui_scale;
	panel_h = scr_h - safe_b - panel_y - footer_h - 8 * ui_scale;
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

// ES system name for the currently highlighted systems-list row
static const char *sel_es_name(void)
{
	if (ui_level != LV_SYSTEMS || !item_cnt || sel < 0 || sel >= item_cnt) return "";
	return es_sys_name(items[sel].name);
}

// (re)load the ES theme for the current browser level and fill `st`
static void theme_apply()
{
	st.use_theme = 0;
	st.showcase = 0;
	st.text = COL_TEXT;
	st.dim = COL_DIM;
	st.sel_bg = COL_SEL_BG;
	st.sel_text = COL_SEL_TEXT;
	st.accent = COL_ACCENT;
	st.font_px = 16 * ui_scale;
	st.list_align = THEME_ALIGN_LEFT;
	st.have_panel = 1;

	theme_unload();
	if (!cfg.fbui_theme[0]) return;

	char base[1024], xml[1300];
	static char es_buf[NAME_LEN];
	snprintf(base, sizeof(base), "%s/themes/%s", getRootDir(), cfg.fbui_theme);

	const char *es = NULL;
	int found = 0, borrowed = 0;
	int want_system = (ui_level == LV_SYSTEMS);

	if (want_system && item_cnt)
		es = sel_es_name();
	else if (cur_sys[0])
	{
		es = es_sys_name(cur_sys);
		snprintf(xml, sizeof(xml), "%s/%s/theme.xml", base, es);
		found = !access(xml, R_OK);
	}
	if (!found)
	{
		snprintf(xml, sizeof(xml), "%s/theme.xml", base);
		found = !access(xml, R_OK);
	}
	if (!found)
	{
		// no matching theme: borrow the first per-system one for global
		// styling (its system-specific logos get hidden below)
		DIR *d = opendir(base);
		if (d)
		{
			struct dirent *de;
			while ((de = readdir(d)))
			{
				if (de->d_name[0] == '.') continue;
				snprintf(xml, sizeof(xml), "%s/%s/theme.xml", base, de->d_name);
				if (!access(xml, R_OK))
				{
					snprintf(es_buf, sizeof(es_buf), "%s", de->d_name);
					es = es_buf;
					found = 1;
					borrowed = 1;
					break;
				}
			}
			closedir(d);
		}
	}
	if (!found) return;

	const char *sysname = want_system && item_cnt ? items[sel].name
	                     : (cur_sys[0] ? cur_sys : (es ? es : ""));
	const char *systheme = es ? es : "";

	int loaded = 0;
	if (want_system)
	{
		// Atlas/Carbon "system" view = full-bleed art + carousel. We keep the
		// art and draw the logo ourselves (carousel is unsupported).
		loaded = theme_load(xml, "system", sysname, systheme, scr_w, scr_h) > 0;
		if (loaded) st.showcase = 1;
	}
	if (!loaded)
		loaded = theme_load(xml, "detailed", sysname, systheme, scr_w, scr_h) > 0
		      || theme_load(xml, "basic", sysname, systheme, scr_w, scr_h) > 0;
	if (!loaded)
	{
		theme_unload();
		return;
	}

	// Two explicit font profiles: 240p is integer-scaled bitmap only; HD keeps
	// the theme's anti-aliased TrueType face.
	theme_set_bitmap_font(ui_lowres);

	if (st.showcase)
	{
		// carousel-driven labels/logo/bg: we paint those per selection;
		// keep only the solid backdrop color from the theme
		theme_set_hidden("logo,system-background,system-manufacturer,system-fullname,"
			"system-year,system-description,system-count,system-count-single,"
			"system-count-null");
		st.font_px = scr_h / 28;
		if (st.font_px < (ui_lowres ? 32 : 12)) st.font_px = (ui_lowres ? 32 : 12);
		st.have_panel = 0;
	}
	else
	{
		theme_set_hidden(borrowed ? "logo,logo2,ControllerOverlay" : "");
		theme_list_style_t ls;
		if (theme_get_list(&ls))
		{
			st.text = ls.primary & 0xFFFFFF;
			st.accent = ls.secondary & 0xFFFFFF;
			st.sel_bg = ls.sel_bar & 0xFFFFFF;
			st.sel_text = ls.sel_text & 0xFFFFFF;
			st.font_px = ls.font_px;
			st.list_align = ls.align;
		}
		// Theme font sizes are designed for 720/1080-line displays.  Make the
		// CJK list glyphs exactly 2x their old 16px 240p size.
		if (ui_lowres && st.font_px < 32) st.font_px = 32;
	}
	st.use_theme = 1;
}

// classic layout, then override geometry with the theme's textlist/md_image
static void layout_apply()
{
	layout_compute();
	if (!st.use_theme) return;
	if (st.showcase)
	{
		// single-item carousel: no on-screen list geometry needed
		list_rows = 1;
		list_x = 0;
		list_y = 0;
		list_w = 0;
		row_h = scr_h;
		st.have_panel = 0;
		return;
	}
	if (ui_lowres)
	{
		// Batocera themes target 16:9 HD screens.  Keep their colors and safe
		// background composition, but use the dedicated three-row 240p browser
		// geometry so every theme remains readable and navigable on a CRT.
		st.have_panel = 1;
		return;
	}

	theme_list_style_t ls;
	if (theme_get_list(&ls) && ls.w > 40 && ls.h > 40)
	{
		list_x = ls.x;
		list_y = ls.y;
		list_w = ls.w;
		row_h = ui_lowres ? 36 : ls.row_h;
		list_rows = ls.h / row_h;
		if (list_rows < 1) list_rows = 1;
	}

	int px, py, pw, ph;
	if (theme_get_image_rect("md_image", &px, &py, &pw, &ph) && pw > 0 && ph > 0)
	{
		panel_x = px;
		panel_y = py;
		panel_w = pw;
		panel_h = ph;
		st.have_panel = 1;
	}
	else
	{
		st.have_panel = 0;
	}
}

// text dispatch: theme TTF engine when a theme is active, bitmap engine else
static int ui_text(int x, int y, const char *s, uint32_t color, int px, int max_w, int align)
{
	if (st.use_theme && !ui_lowres)
	{
		int w = theme_draw_text(shadow, scr_w, scr_h, x, y, s, color, px, max_w, align);
		dirty_add(x, y, x + ((max_w > 0) ? max_w : w), y + px + px / 4 + 1);
		return w;
	}

	int sc = ui_lowres ? 2 : ((px >= 28) ? 2 : 1);
	if (max_w > 0 && align != THEME_ALIGN_LEFT)
	{
		int tw = text_width(s, sc);
		if (tw < max_w) x += (align == THEME_ALIGN_CENTER) ? (max_w - tw) / 2 : (max_w - tw);
	}
	return draw_text(x, y, s, color, sc, max_w);
}

static int classic_text_scale()
{
	return ui_lowres ? 2 : ui_scale;
}

// render the static layer: theme elements, or the classic vertical gradient
static void render_bglayer()
{
	if (st.use_theme)
	{
		theme_render_static(bglayer, scr_w, scr_h);
		return;
	}

	int r0 = (COL_BG_TOP >> 16) & 0xFF, g0 = (COL_BG_TOP >> 8) & 0xFF, b0 = COL_BG_TOP & 0xFF;
	int r1 = (COL_BG_BOT >> 16) & 0xFF, g1 = (COL_BG_BOT >> 8) & 0xFF, b1 = COL_BG_BOT & 0xFF;
	for (int y = 0; y < scr_h; y++)
	{
		int r = r0 + (r1 - r0) * y / scr_h;
		int g = g0 + (g1 - g0) * y / scr_h;
		int b = b0 + (b1 - b0) * y / scr_h;
		uint32_t c = (uint32_t)((r << 16) | (g << 8) | b);
		uint32_t *p = bglayer + (size_t)y * scr_w;
		for (int x = 0; x < scr_w; x++) *p++ = c;
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

// theme reload guard: only re-parse XML when theme name or system changes
static char theme_key[NAME_LEN + 80] = "\x01";

static void theme_refresh()
{
	char key[sizeof(theme_key)];
	// showcase swaps art per selection without re-parsing the theme XML
	snprintf(key, sizeof(key), "%s|%d|%s", cfg.fbui_theme, ui_level, cur_sys);
	if (strcmp(key, theme_key))
	{
		snprintf(theme_key, sizeof(theme_key), "%s", key);
		theme_apply();
	}
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
	theme_refresh();
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
		theme_refresh();
		need_full_redraw = 1;
		return;
	}
	char path[2048];
	cur_dir_path(path, sizeof(path));
	scan_dir(path, 1);
	gamelist_apply_names(path);
	theme_refresh();
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
	theme_blit_image_file(shadow, scr_w, scr_h, "/tmp/fbui_prev.png", bx, by, bw, bh);
	dirty_add(bx, by, bx + bw, by + bh);
	return 1;
}

// blend cover (keeping aspect) into the panel area of the shadow buffer
static void draw_cover(const char *file, int box_x, int box_y, int box_w, int box_h)
{
	Imlib_Image img = imlib_load_image(file);
	if (!img) return;

	imlib_context_set_image(img);
	int sw = imlib_image_get_width();
	int sh = imlib_image_get_height();
	if (sw <= 0 || sh <= 0) { imlib_free_image(); return; }

	int dw = box_w, dh = sh * box_w / sw;
	if (dh > box_h) { dh = box_h; dw = sw * box_h / sh; }
	int dx = box_x + (box_w - dw) / 2;
	int dy = box_y + (box_h - dh) / 2;

	Imlib_Image dst = imlib_create_image_using_data(scr_w, scr_h, shadow);
	if (dst)
	{
		imlib_context_set_image(dst);
		imlib_blend_image_onto_image(img, 0, 0, 0, sw, sh, dx, dy, dw, dh);
		imlib_free_image(); // frees dst wrapper, not shadow
		dirty_add(dx, dy, dx + dw, dy + dh);
	}

	imlib_context_set_image(img);
	imlib_free_image();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void render_header()
{
	if (st.showcase) return; // counter drawn inside render_showcase

	if (st.use_theme)
	{
		int px = st.font_px;
		int m = scr_w / 60 + 4;
		if (ui_lowres) m = safe_l + 4;

		// title, centered in the top band (the theme's logo slot region;
		// empty at systems level). Top-left is off limits: themes like
		// Atlas put the game-art box there and render_panel would erase
		// anything below its top edge.
		if (ui_level == LV_SYSTEMS)
		{
			// Keep the 240p title at the same readable 2x CJK size as rows.
			// Scaling it again would collide with the first list row.
			int tpx = ui_lowres ? 32 : px + px / 3;
			int ty = scr_h * 74 / 1000 - tpx / 2;
			if (ui_lowres) ty = safe_t + 2;
			else if (ty < 4) ty = 4;
			int tw = ui_lowres ? (scr_w - safe_l - safe_r) : (scr_w * 44 / 100);
			int tx = ui_lowres ? safe_l : (scr_w - tw) / 2;
			bg_restore(tx - 4, 0, tw + 8, scr_h * 13 / 100);
			ui_text(tx, ty, "游戏库 GAME LIBRARY", 0xE0E0E0, tpx, tw, THEME_ALIGN_CENTER);
		}

		// At 240p the 2x title uses the whole safe top band; the counter would
		// overlap it, so the selected row itself remains the only indicator.
		if (!ui_lowres)
		{
			int bw = px * 8;
			char cnt[48];
			snprintf(cnt, sizeof(cnt), "%d / %d", item_cnt ? sel + 1 : 0, item_cnt);
			bg_restore(scr_w - bw - m, m / 2, bw, px + px / 4 + 2);
			ui_text(scr_w - bw - m, m / 2, cnt, st.dim, px, bw, THEME_ALIGN_RIGHT);
		}
		return;
	}

	int hx = ui_lowres ? safe_l : 0;
	int hy = ui_lowres ? safe_t : 0;
	int hw = ui_lowres ? (scr_w - safe_l - safe_r) : scr_w;
	int ts = classic_text_scale();
	fill_rect(hx, hy, hw, header_h, COL_HEADER);
	fill_rect(hx, hy + header_h - 2 * ui_scale, hw, 2 * ui_scale, COL_ACCENT);

	int ty = hy + (header_h - 16 * ts) / 2;
	if (ui_level == LV_SYSTEMS)
	{
		draw_text(list_x, ty, "游戏库 GAME LIBRARY", 0xFFFFFF, ts, scr_w / 2);
	}
	else
	{
		char t[300];
		if (cur_rel[0]) snprintf(t, sizeof(t), "%s / %s", cur_sys, cur_rel);
		else snprintf(t, sizeof(t), "%s", cur_sys);
		draw_text(list_x, ty, t, 0xFFFFFF, ts, scr_w / 2);
	}

	char cnt[48];
	snprintf(cnt, sizeof(cnt), "%d / %d", item_cnt ? sel + 1 : 0, item_cnt);
	int cw = text_width(cnt, ts);
	draw_text(scr_w - safe_r - cw - 8, ty, cnt, COL_DIM, ts, 0);
}

static void render_footer()
{
	int show_msg_now = (msg_text[0] && !CheckTimer(msg_timer));
	if (!show_msg_now) msg_text[0] = 0;

	if (st.use_theme)
	{
		int hx, hy, hpx;
		uint32_t hc;
		if (!theme_get_help(&hx, &hy, &hc, &hpx))
		{
			hpx = st.font_px * 4 / 5;
			hx = scr_w / 60 + 4;
			hy = scr_h - hpx * 2;
			hc = st.dim;
		}
		if (ui_lowres)
		{
			// Short labels leave enough width for a full 2x (32px) CJK footer.
			hpx = 32;
			hx = safe_l;
			hy = scr_h - safe_b - hpx - 2;
		}
		if (hpx < 10) hpx = 10;
		bg_restore(0, hy - 2, scr_w, scr_h - hy + 2);

		if (show_msg_now)
		{
			ui_text(hx, hy, msg_text, 0xFFB040, hpx, scr_w - hx - 8, THEME_ALIGN_LEFT);
			return;
		}

		// icon + label pairs (Atlas help icons; fall back to plain text)
		struct { const char *icon; const char *label; } tips[3];
		int ntips = 0;
		if (ui_level == LV_SYSTEMS)
		{
			tips[ntips++] = { "a", "进入" };
			tips[ntips++] = { "start", "经典菜单" };
		}
		else
		{
			tips[ntips++] = { "a", "启动" };
			tips[ntips++] = { "b", "返回" };
			tips[ntips++] = { "start", "经典菜单" };
		}

		int x = hx;
		int ih = hpx + 4;
		for (int i = 0; i < ntips; i++)
		{
			char ipath[1300];
			snprintf(ipath, sizeof(ipath), "%s/themes/%s/assets/icons/help/switch/%s.png",
				getRootDir(), cfg.fbui_theme, tips[i].icon);
			if (access(ipath, R_OK))
				snprintf(ipath, sizeof(ipath), "%s/themes/%s/assets/icons/help/xboxone/%s.png",
					getRootDir(), cfg.fbui_theme, tips[i].icon);
			if (!access(ipath, R_OK))
			{
				theme_blit_image_file(shadow, scr_w, scr_h, ipath, x, hy - 2, ih, ih);
				dirty_add(x, hy - 2, x + ih, hy - 2 + ih);
				x += ih + 4;
			}
			int tw = ui_text(x, hy, tips[i].label, hc & 0xFFFFFF, hpx, 0, THEME_ALIGN_LEFT);
			x += tw + hpx;
		}
		return;
	}

	const char *hint = ui_lowres
		? ((ui_level == LV_SYSTEMS) ? "A 进入   START 菜单" : "A 启动   B 返回   START 菜单")
		: ((ui_level == LV_SYSTEMS) ? "ENTER 进入   F12/ESC 经典菜单"
		                              : "ENTER 启动   BACKSPACE 返回   F12/ESC 经典菜单");
	int y = scr_h - safe_b - footer_h;
	int fx = ui_lowres ? safe_l : 0;
	int fw = ui_lowres ? (scr_w - safe_l - safe_r) : scr_w;
	int ts = classic_text_scale();
	fill_rect(fx, y, fw, footer_h, COL_HEADER);
	int ty = y + (footer_h - 16 * ts) / 2;
	if (show_msg_now)
		draw_text(list_x, ty, msg_text, 0xFFB040, ts, scr_w - safe_r - list_x);
	else
		draw_text(list_x, ty, hint, COL_DIM, ts, scr_w - safe_r - list_x);
}

static void render_row(int row)
{
	int idx = scroll_top + row;
	int y = list_y + row * row_h;

	bg_restore(list_x, y, list_w, row_h);
	if (idx == sel) fill_rect(list_x, y, list_w, row_h, st.sel_bg);

	if (idx >= item_cnt) return;

	entry_t *e = &items[idx];
	uint32_t fg = (idx == sel) ? st.sel_text : (e->is_dir ? st.accent : st.text);

	int px = st.use_theme ? st.font_px : (ui_lowres ? 32 : 16 * ui_scale);
	int pad = st.use_theme ? (row_h / 4 + 2) : (ui_lowres ? 4 : 8 * ui_scale);
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
		int px = st.use_theme ? st.font_px : (ui_lowres ? 32 : 16 * ui_scale);
		ui_text(list_x + 8 * ui_scale, list_y + row_h, "（空目录）", st.dim, px, list_w, THEME_ALIGN_LEFT);
	}
}

// Atlas/Carbon "system" view: full-bleed background + centered logo (carousel substitute)
static int find_system_asset(const char *es, const char *fallback_name,
	char *out, size_t sz, int want_logo)
{
	static const char *logo_cands[] = {
		"%s/themes/%s/assets/systems/logos/%s-w.svg",
		"%s/themes/%s/assets/systems/logos/%s-w.png",
		"%s/themes/%s/assets/systems/logos/%s.svg",
		"%s/themes/%s/assets/systems/logos/%s.png",
		"%s/themes/%s/%s/art/system.svg",
		"%s/themes/%s/%s/art/system.png",
	};
	static const char *bg_cands[] = {
		"%s/themes/%s/assets/systems/background/%s.jpg",
		"%s/themes/%s/assets/systems/background/%s.png",
		"%s/themes/%s/%s/art/background.jpg",
		"%s/themes/%s/%s/art/background.png",
	};
	const char **cands = want_logo ? logo_cands : bg_cands;
	size_t n = want_logo ? sizeof(logo_cands)/sizeof(logo_cands[0])
	                     : sizeof(bg_cands)/sizeof(bg_cands[0]);

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

	for (int ni = 0; ni < nnames; ni++)
	{
		for (size_t i = 0; i < n; i++)
		{
			snprintf(out, sz, cands[i], getRootDir(), cfg.fbui_theme, names[ni]);
			if (!access(out, R_OK)) return 1;
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

static void sc_slot_clear(sc_slot_t *s)
{
	if (s->bg) sc_cache_bytes -= (size_t)s->bg_w * s->bg_h * 4;
	if (s->logo) sc_cache_bytes -= (size_t)s->logo_w * s->logo_h * 4;
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
	// Size the cache from what Linux can really use, not the board's nominal
	// 1 GiB. MiSTer commonly exposes only about 492 MiB to the HPS kernel.
	// Keep at least 256 MiB available for the rest of the system.
	const size_t slot_bytes = (size_t)SC_BG_MAX_W * SC_BG_MAX_H * 4
		+ (size_t)SC_LOGO_MAX_W * SC_LOGO_MAX_H * 4;
	size_t avail = sc_mem_available();
	// Add our existing cache back when returning from a game list, otherwise
	// MemAvailable would make the limit shrink and strand upper cache slots.
	size_t effective_avail = avail + sc_cache_bytes;
	size_t budget = (effective_avail > 256U * 1024 * 1024)
		? effective_avail - 256U * 1024 * 1024 : 64U * 1024 * 1024;
	if (budget > 192U * 1024 * 1024) budget = 192U * 1024 * 1024;
	sc_slot_limit = (int)(budget / slot_bytes);
	if (sc_slot_limit < 5) sc_slot_limit = 5;
	if (sc_slot_limit > SC_CACHE_SLOTS) sc_slot_limit = SC_CACHE_SLOTS;
	sc_prefetch_idx = sel + 1;
	if (sc_prefetch_idx >= item_cnt) sc_prefetch_idx = 0;
	sc_prefetch_remaining = item_cnt - 1;
	if (sc_prefetch_remaining > sc_slot_limit - 1)
		sc_prefetch_remaining = sc_slot_limit - 1;
	printf("FBUI: system art cache budget %.1f MiB (%d slots), MemAvailable %.1f MiB\n",
		(sc_slot_limit * slot_bytes) / 1048576.0, sc_slot_limit,
		avail / 1048576.0);
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
		for (int x = 0; x < dw; x++) drow[x] = srow[x * sw / dw];
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
	if (!s->tried_bg)
	{
		s->tried_bg = 1;
		if (find_system_asset(es, fallback_name, path, sizeof(path), 0))
		{
			s->bg = theme_load_cover_rgb(path, SC_BG_MAX_W, SC_BG_MAX_H, SC_BG_TINT, &s->bg_w, &s->bg_h);
			if (s->bg) sc_cache_bytes += (size_t)s->bg_w * s->bg_h * 4;
		}
		return 1;
	}
	if (!s->tried_logo)
	{
		s->tried_logo = 1;
		if (find_system_asset(es, fallback_name, path, sizeof(path), 1))
		{
			s->logo = theme_load_fit_rgb(path, SC_LOGO_MAX_W, SC_LOGO_MAX_H, &s->logo_w, &s->logo_h);
			if (s->logo) sc_cache_bytes += (size_t)s->logo_w * s->logo_h * 4;
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
	int ny = ly + lh + (ui_lowres ? 2 : scr_h * 2 / 100);
	if (!have_logo)
	{
		int px = ui_lowres ? 32 : st.font_px * 2;
		ui_text(lx, ly + lh / 2 - px / 2, label, 0xFFFFFF, px, lw, THEME_ALIGN_CENTER);
	}
	int npx = ui_lowres ? 32 : st.font_px + st.font_px / 4;
	ui_text(lx, ny, label, 0xF0F0F0, npx, lw, THEME_ALIGN_CENTER);
	ny += npx + (ui_lowres ? 2 : st.font_px / 2);
	char cnt[48];
	snprintf(cnt, sizeof(cnt), "%d / %d", sel + 1, item_cnt);
	int cpx = ui_lowres ? 16 : st.font_px;
	ui_text(scr_w / 4, ny, cnt, 0xC0C0C0, cpx, scr_w / 2, THEME_ALIGN_CENTER);
}

// Lightweight scrub while the stick is moving: only refresh the title strip.
// Avoids a full 1080p dirty flush on every tick (that was the main stall).
static void render_showcase_scrub(void)
{
	if (!item_cnt || sel >= item_cnt) return;
	entry_t *e = &items[sel];
	int lx, ly, lw, lh;
	showcase_geom(&lx, &ly, &lw, &lh);
	int npx = ui_lowres ? 32 : st.font_px + st.font_px / 4;
	int ty = ly + lh + (ui_lowres ? 2 : scr_h * 2 / 100);
	int cpx = ui_lowres ? 16 : st.font_px;
	int gap = ui_lowres ? 2 : st.font_px / 2;
	int th = npx + gap + cpx + (ui_lowres ? 2 : scr_h / 50);
	if (ty + th > scr_h) th = scr_h - ty;
	if (th < 1) return;

	bg_restore(0, ty, scr_w, th);
	const char *label = e->disp[0] ? e->disp : e->name;
	ui_text(lx, ty, label, 0xF0F0F0, npx, lw, THEME_ALIGN_CENTER);
	char cnt[48];
	snprintf(cnt, sizeof(cnt), "%d / %d", sel + 1, item_cnt);
	ui_text(scr_w / 4, ty + npx + gap, cnt, 0xC0C0C0, cpx, scr_w / 2, THEME_ALIGN_CENTER);
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

static void render_panel()
{
	if (st.showcase) return;
	if (st.use_theme)
	{
		// md_image slot from the theme, or a default right-side box
		int bx = panel_x, by = panel_y, bw = panel_w, bh = panel_h;
		if (!st.have_panel)
		{
			int sw = scr_w - safe_l - safe_r;
			int sh = scr_h - safe_t - safe_b;
			bx = safe_l + sw * 56 / 100;
			by = safe_t + sh * 28 / 100;
			bw = sw * 38 / 100;
			bh = sh * 44 / 100;
		}
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
				if (!preview_blit(bx, by, bw, bh) && cover[0])
				{
					theme_blit_image_file(shadow, scr_w, scr_h, cover, bx, by, bw, bh);
					dirty_add(bx, by, bx + bw, by + bh);
				}
			}
			else
			{
				preview_stop();
				if (cover[0])
				{
					theme_blit_image_file(shadow, scr_w, scr_h, cover, bx, by, bw, bh);
					dirty_add(bx, by, bx + bw, by + bh);
				}
				// no media → leave the theme background empty
			}
		}
		else if (ui_level == LV_SYSTEMS)
		{
			// show the selected system's theme logo; layouts differ per theme
			// (Carbon: <es>/art/system.svg, Atlas: assets/systems/logos/<es>.svg)
			static const char *cand[] = {
				"%s/themes/%s/%s/art/system.svg",
				"%s/themes/%s/%s/art/system.png",
				"%s/themes/%s/assets/systems/logos/%s-w.svg",
				"%s/themes/%s/assets/systems/logos/%s.svg",
				"%s/themes/%s/assets/systems/logos/%s.png",
			};
			char p[1300];
			const char *es = es_sys_name(e->name);
			p[0] = 0;
			for (size_t ci = 0; ci < sizeof(cand) / sizeof(cand[0]); ci++)
			{
				snprintf(p, sizeof(p), cand[ci], getRootDir(), cfg.fbui_theme, es);
				if (!access(p, R_OK)) break;
				p[0] = 0;
			}

			if (p[0])
			{
				theme_blit_image_file(shadow, scr_w, scr_h, p, bx, by, bw, bh);
				dirty_add(bx, by, bx + bw, by + bh);
			}
			else
			{
				// no art for this system: large dim name instead
				ui_text(bx, by + bh / 2 - st.font_px, e->name, st.dim,
					st.font_px * 2, bw, THEME_ALIGN_CENTER);
			}
		}
		return;
	}

	fill_rect(panel_x, panel_y, panel_w, panel_h, COL_PANEL);
	fill_rect(panel_x, panel_y, panel_w, 2 * ui_scale, COL_ACCENT);

	if (!item_cnt || sel >= item_cnt) return;
	entry_t *e = &items[sel];

	int pad = ui_lowres ? 4 : 10 * ui_scale;
	int name_h = ui_lowres ? 36 : 40 * ui_scale;
	int box_x = panel_x + pad;
	int box_y = panel_y + pad;
	int box_w = panel_w - 2 * pad;
	int box_h = panel_h - name_h - 3 * pad;

	if (ui_level == LV_GAMES && !e->is_dir)
	{
		char cover[2400] = "", video[2400] = "";
		find_media(e->name, cover, sizeof(cover), video, sizeof(video));
		if (video[0])
		{
			preview_start(video, box_x, box_y, box_w, box_h);
			if (!preview_blit(box_x, box_y, box_w, box_h) && cover[0])
				draw_cover(cover, box_x, box_y, box_w, box_h);
		}
		else
		{
			preview_stop();
			if (cover[0])
				draw_cover(cover, box_x, box_y, box_w, box_h);
			// no media → empty box
		}

		// file size
		char full[2400], dir[2048];
		cur_dir_path(dir, sizeof(dir));
		snprintf(full, sizeof(full), "%s/%s", dir, e->name);
		struct stat fst;
		char info[64] = "";
		if (!stat(full, &fst))
		{
			if (fst.st_size >= 1024 * 1024) snprintf(info, sizeof(info), "%.1f MB", fst.st_size / 1048576.0);
			else snprintf(info, sizeof(info), "%ld KB", (long)(fst.st_size / 1024));
		}
		int iy = panel_y + panel_h - name_h - pad / 2;
		draw_text(box_x, iy, info, COL_DIM, ui_scale, box_w);
	}
	else
	{
		// big label for system / directory
		fill_rect(box_x, box_y, box_w, box_h, 0x0A0D12);
		int tw = text_width(e->name, ui_scale * 2);
		if (tw > box_w) tw = box_w;
		draw_text(box_x + (box_w - tw) / 2, box_y + box_h / 2 - 16 * ui_scale,
			e->name, COL_ACCENT, ui_scale * 2, box_w);
	}

	// name (bottom of panel)
	int ny = panel_y + panel_h - name_h + pad / 2;
	draw_text(box_x, ny, e->disp[0] ? e->disp : e->name, COL_TEXT,
		classic_text_scale(), box_w);
}

static void render_full()
{
	render_bglayer();
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
	layout_compute();
	theme_key[0] = 1; theme_key[1] = 0; // force theme reload (resolution may differ)
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
	else if (!showcase_hold && !nav_held && st.showcase
	         && !need_full_redraw && !need_list_redraw && !need_panel_redraw)
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
