// FBUI theme engine — EmulationStation / Batocera theme.xml subset.
// See fbui_theme.h for the supported feature list.
//
// Parsing uses the bundled sxmlc DOM API. Rendering targets a plain
// 0RGB32 buffer (the FBUI shadow buffer): TTF text via stb_truetype
// (glyph cache, unifont fallback for missing glyphs), SVG via nanosvg,
// PNG/JPG via Imlib2.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>

#include "sxmlc.h"
#include "fbui.h"
#include "fbui_theme.h"
#include "font_cjk.h"
#include "lib/imlib2/Imlib2.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "lib/stb/stb_truetype.h"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "lib/nanosvg/nanosvg.h"
#include "lib/nanosvg/nanosvgrast.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------
// Element / variable storage
// ---------------------------------------------------------------------------

enum { EL_IMAGE, EL_TEXT, EL_TEXTLIST, EL_HELP };

struct th_el_t
{
	int type;
	char name[64];
	int extra;

	float pos_x, pos_y;          // normalized
	float size_w, size_h;        // normalized, 0 = auto
	float maxs_w, maxs_h;        // normalized, 0 = unset
	float mins_w, mins_h;        // normalized, 0 = unset (fill box, crop)
	float org_x, org_y;
	float z;
	int visible;
	int dynamic;                 // bound to {game:}/{system:} data we don't have
	int is_gameart;              // image whose path was {game:image/thumbnail}
	int tile;                    // image: repeat at native size
	int has_size;                // explicit <size> given -> stretch
	int has_min;                 // explicit <minSize> given -> cover/crop
	uint32_t tint;               // image color modulation (0xAARRGGBB)

	char path[512];              // absolute image path
	char text[256];
	char font_path[512];         // absolute TTF path
	float font_size;             // fraction of screen height
	float line_spacing;
	int align;

	uint32_t color;              // text color / helpsystem textColor
	uint32_t sel_bar, sel_text, secondary; // textlist
};

#define TH_MAX_EL   96
#define TH_MAX_VARS 96

static th_el_t els[TH_MAX_EL];
static int el_cnt = 0;

struct th_var_t { char name[64]; char val[512]; };
static th_var_t vars[TH_MAX_VARS];
static int var_cnt = 0;

static char want_view[32];
static int scrW = 0, scrH = 0;
static int th_loaded = 0;
static char main_font[512]; // font of the textlist (or first text el)
static char hidden_csv[128] = "";
static int bitmap_font = 0;

static void trim(char *s);

// ES <subset> option groups: the first <include> of each group is the default
struct th_subset_t { char name[32]; char chosen[32]; };
#define TH_MAX_SUBSETS 24
static th_subset_t subsets[TH_MAX_SUBSETS];
static int subset_cnt = 0;

static const char *subset_chosen(const char *name)
{
	for (int i = 0; i < subset_cnt; i++)
		if (!strcasecmp(subsets[i].name, name)) return subsets[i].chosen;
	return NULL;
}

static const char *attr_get(const XMLNode *n, const char *name)
{
	for (int i = 0; i < n->n_attributes; i++)
		if (!strcmp(n->attributes[i].name, name)) return n->attributes[i].value;
	return NULL;
}

// ifSubset="group:val1|val2" -> keep node only when the group's chosen
// option is one of the listed values
static int if_subset_ok(const XMLNode *n)
{
	const char *cond = attr_get(n, "ifSubset");
	if (!cond) return 1;

	char buf[128];
	snprintf(buf, sizeof(buf), "%s", cond);
	char *colon = strchr(buf, ':');
	if (!colon) return 1;
	*colon = 0;

	const char *chosen = subset_chosen(buf);
	if (!chosen) return 0;

	char *save = NULL;
	for (char *tok = strtok_r(colon + 1, "|", &save); tok; tok = strtok_r(NULL, "|", &save))
	{
		trim(tok);
		if (!strcasecmp(tok, chosen)) return 1;
	}
	return 0;
}

void theme_set_hidden(const char *names_csv)
{
	snprintf(hidden_csv, sizeof(hidden_csv), ",%s,", names_csv ? names_csv : "");
}

static int el_hidden(const char *name)
{
	char pat[80];
	snprintf(pat, sizeof(pat), ",%s,", name);
	return strstr(hidden_csv, pat) != NULL;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void var_set(const char *name, const char *val)
{
	for (int i = 0; i < var_cnt; i++)
	{
		if (!strcmp(vars[i].name, name))
		{
			snprintf(vars[i].val, sizeof(vars[i].val), "%s", val);
			return;
		}
	}
	if (var_cnt >= TH_MAX_VARS) return;
	snprintf(vars[var_cnt].name, sizeof(vars[var_cnt].name), "%s", name);
	snprintf(vars[var_cnt].val, sizeof(vars[var_cnt].val), "%s", val);
	var_cnt++;
}

static const char *var_get(const char *name)
{
	for (int i = 0; i < var_cnt; i++)
		if (!strcmp(vars[i].name, name)) return vars[i].val;
	return "";
}

// replace ${var} occurrences
static void subst_vars(const char *in, char *out, size_t sz)
{
	size_t o = 0;
	while (*in && o + 1 < sz)
	{
		if (in[0] == '$' && in[1] == '{')
		{
			const char *end = strchr(in + 2, '}');
			if (end)
			{
				char name[64];
				size_t nl = (size_t)(end - in - 2);
				if (nl >= sizeof(name)) nl = sizeof(name) - 1;
				memcpy(name, in + 2, nl);
				name[nl] = 0;
				const char *v = var_get(name);
				while (*v && o + 1 < sz) out[o++] = *v++;
				in = end + 1;
				continue;
			}
		}
		out[o++] = *in++;
	}
	out[o] = 0;
}

static void trim(char *s)
{
	char *p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	size_t l = strlen(p);
	while (l && isspace((unsigned char)p[l - 1])) l--;
	memmove(s, p, l);
	s[l] = 0;
}

// resolve a (possibly relative) path against base_dir
static void resolve_path(const char *val, const char *base_dir, char *out, size_t sz)
{
	if (val[0] == '/') { snprintf(out, sz, "%s", val); return; }
	if (val[0] == '.' && val[1] == '/') val += 2;
	snprintf(out, sz, "%s/%s", base_dir, val);
}

// resolve against dir; missing .webp assets may ship as converted .png
static int try_asset(const char *val, const char *dir, char *out, size_t sz)
{
	resolve_path(val, dir, out, sz);
	if (!access(out, R_OK)) return 1;
	char *dot = strrchr(out, '.');
	if (dot && !strcasecmp(dot, ".webp") && (size_t)(dot - out) + 5 < sz)
	{
		strcpy(dot, ".png");
		if (!access(out, R_OK)) return 1;
	}
	return 0;
}

// ES themes are inconsistent about relative paths: some are relative to the
// containing xml (Carbon), some to the theme root (Atlas variables). Try both.
static int resolve_asset(const char *val, const char *base_dir, char *out, size_t sz)
{
	if (try_asset(val, base_dir, out, sz)) return 1;
	const char *root = var_get("themePath");
	if (root[0] && try_asset(val, root, out, sz)) return 1;
	return 0;
}

// "RRGGBB" or "RRGGBBAA" -> 0xAARRGGBB
static uint32_t parse_color(const char *s)
{
	while (*s && isspace((unsigned char)*s)) s++;
	uint32_t v = (uint32_t)strtoul(s, NULL, 16);
	int digits = 0;
	for (const char *p = s; isxdigit((unsigned char)*p); p++) digits++;
	if (digits > 6) return ((v & 0xFF) << 24) | (v >> 8); // RGBA -> ARGB
	return 0xFF000000 | v;
}

static void parse_pair(const char *s, float *a, float *b)
{
	char *end = NULL;
	*a = strtof(s, &end);
	if (end) *b = strtof(end, NULL);
}

static int parse_align(const char *s)
{
	if (strcasestr(s, "center")) return THEME_ALIGN_CENTER;
	if (strcasestr(s, "right")) return THEME_ALIGN_RIGHT;
	return THEME_ALIGN_LEFT;
}

// ---------------------------------------------------------------------------
// XML parsing
// ---------------------------------------------------------------------------

static th_el_t *el_get(int type, const char *name)
{
	for (int i = 0; i < el_cnt; i++)
		if (els[i].type == type && !strcmp(els[i].name, name)) return &els[i];

	if (el_cnt >= TH_MAX_EL) return NULL;
	th_el_t *e = &els[el_cnt++];
	memset(e, 0, sizeof(*e));
	e->type = type;
	snprintf(e->name, sizeof(e->name), "%s", name);
	e->visible = 1;
	e->line_spacing = 1.5f;
	e->font_size = 0.045f;
	e->z = (type == EL_IMAGE) ? 30.0f : 40.0f;
	e->color = 0xFF777777;
	e->sel_bar = 0xFF303C50;
	e->sel_text = 0xFFFFFFFF;
	e->secondary = 0xFF888888;
	e->tint = 0xFFFFFFFF;
	return e;
}

static void node_text(const XMLNode *n, char *out, size_t sz)
{
	char raw[1024] = "";
	if (n->text) snprintf(raw, sizeof(raw), "%s", n->text);
	trim(raw);
	subst_vars(raw, out, sz);
	trim(out);
}

// visible can hold ES expressions; we only have static data, so guess:
// metadata-dependent expressions hide the element unless they explicitly
// test for the *absence* of metadata (empty(...)), which is our situation.
static int eval_visible(const char *val)
{
	if (!strcasecmp(val, "false")) return 0;
	if (!strcasecmp(val, "true")) return 1;
	if (strstr(val, "{game:") || strstr(val, "{system:"))
		return !strncasecmp(val, "empty(", 6) ? 1 : 0;
	return 1;
}

static void parse_el_props(XMLNode *n, th_el_t *e, const char *base_dir)
{
	for (int i = 0; i < n->n_children; i++)
	{
		XMLNode *p = n->children[i];
		if (!p->tag || (p->tag_type != TAG_FATHER && p->tag_type != TAG_SELF)) continue;
		if (!if_subset_ok(p)) continue;

		char val[1024];
		node_text(p, val, sizeof(val));

		if (!strcmp(p->tag, "pos"))              parse_pair(val, &e->pos_x, &e->pos_y);
		else if (!strcmp(p->tag, "size"))        { parse_pair(val, &e->size_w, &e->size_h); e->has_size = 1; }
		else if (!strcmp(p->tag, "maxSize"))     parse_pair(val, &e->maxs_w, &e->maxs_h);
		else if (!strcmp(p->tag, "minSize"))     { parse_pair(val, &e->mins_w, &e->mins_h); e->has_min = 1; }
		else if (!strcmp(p->tag, "origin"))      parse_pair(val, &e->org_x, &e->org_y);
		else if (!strcmp(p->tag, "zIndex"))      e->z = strtof(val, NULL);
		else if (!strcmp(p->tag, "visible"))     e->visible = eval_visible(val);
		else if (!strcmp(p->tag, "tile"))        e->tile = !strcasecmp(val, "true");
		else if (!strcmp(p->tag, "x"))           e->pos_x = strtof(val, NULL);
		else if (!strcmp(p->tag, "y"))           e->pos_y = strtof(val, NULL);
		else if (!strcmp(p->tag, "path"))
		{
			// dynamic bindings can't be resolved; remember game art slots
			if (strchr(val, '{'))
			{
				if (strstr(val, "game:image") || strstr(val, "game:thumbnail"))
					e->is_gameart = 1;
				continue;
			}
			// candidate list: the last one that exists on disk wins
			char cand[512];
			if (resolve_asset(val, base_dir, cand, sizeof(cand)))
				snprintf(e->path, sizeof(e->path), "%s", cand);
		}
		else if (!strcmp(p->tag, "text"))
		{
			if (strstr(val, "{game:") || strstr(val, "{system:")) { e->dynamic = 1; continue; }
			snprintf(e->text, sizeof(e->text), "%s", val);
		}
		else if (!strcmp(p->tag, "fontPath"))    resolve_asset(val, base_dir, e->font_path, sizeof(e->font_path));
		else if (!strcmp(p->tag, "fontSize"))    e->font_size = strtof(val, NULL);
		else if (!strcmp(p->tag, "lineSpacing")) e->line_spacing = strtof(val, NULL);
		else if (!strcmp(p->tag, "alignment") || !strcmp(p->tag, "horizontalAlignment"))
			e->align = parse_align(val);
		else if (!strcmp(p->tag, "color") || !strcmp(p->tag, "textColor"))
		{
			if (e->type == EL_IMAGE) e->tint = parse_color(val);
			else e->color = parse_color(val);
		}
		else if (!strcmp(p->tag, "selectorColor"))  e->sel_bar = parse_color(val);
		else if (!strcmp(p->tag, "selectedColor"))  e->sel_text = parse_color(val);
		else if (!strcmp(p->tag, "primaryColor"))   e->color = parse_color(val);
		else if (!strcmp(p->tag, "secondaryColor")) e->secondary = parse_color(val);
	}
}

// element "name" attribute may hold a comma separated list
static void parse_element(XMLNode *n, int type, const char *base_dir)
{
	char names[256] = "default";
	int extra = 0;

	if (!if_subset_ok(n)) return;

	for (int i = 0; i < n->n_attributes; i++)
	{
		if (!strcmp(n->attributes[i].name, "name"))
			snprintf(names, sizeof(names), "%s", n->attributes[i].value);
		else if (!strcmp(n->attributes[i].name, "extra"))
			extra = !strcasecmp(n->attributes[i].value, "true");
	}

	char *save = NULL;
	for (char *tok = strtok_r(names, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
	{
		char name[64];
		snprintf(name, sizeof(name), "%s", tok);
		trim(name);
		th_el_t *e = el_get(type, name);
		if (!e) return;
		e->extra |= extra;
		parse_el_props(n, e, base_dir);
	}
}

static int view_matches(const char *namelist, const char *want)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", namelist);
	char *save = NULL;
	for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
	{
		trim(tok);
		if (!strcasecmp(tok, want)) return 1;
	}
	return 0;
}

static void parse_view(XMLNode *v, const char *base_dir)
{
	for (int i = 0; i < v->n_children; i++)
	{
		XMLNode *n = v->children[i];
		if (!n->tag || (n->tag_type != TAG_FATHER && n->tag_type != TAG_SELF)) continue;

		if (!strcmp(n->tag, "image"))         parse_element(n, EL_IMAGE, base_dir);
		else if (!strcmp(n->tag, "text"))     parse_element(n, EL_TEXT, base_dir);
		else if (!strcmp(n->tag, "textlist")) parse_element(n, EL_TEXTLIST, base_dir);
		else if (!strcmp(n->tag, "helpsystem")) parse_element(n, EL_HELP, base_dir);
		// video/carousel/rating/datetime/ninepatch: unsupported, skipped
	}
}

static void parse_file(const char *path, int depth)
{
	if (depth > 8) return;

	XMLDoc doc;
	XMLDoc_init(&doc);
	if (!XMLDoc_parse_file_DOM_text_as_nodes(path, &doc, 0))
	{
		printf("FBUI theme: parse failed: %s\n", path);
		XMLDoc_free(&doc);
		return;
	}
	if (doc.i_root < 0)
	{
		XMLDoc_free(&doc);
		return;
	}

	char base_dir[512];
	snprintf(base_dir, sizeof(base_dir), "%s", path);
	char *slash = strrchr(base_dir, '/');
	if (slash) *slash = 0; else snprintf(base_dir, sizeof(base_dir), ".");

	XMLNode *root = doc.nodes[doc.i_root];
	for (int i = 0; i < root->n_children; i++)
	{
		XMLNode *n = root->children[i];
		if (!n->tag || (n->tag_type != TAG_FATHER && n->tag_type != TAG_SELF)) continue;

		if (!strcmp(n->tag, "include"))
		{
			if (!if_subset_ok(n)) continue;
			char val[1024], inc[1024];
			node_text(n, val, sizeof(val));
			if (val[0])
			{
				resolve_path(val, base_dir, inc, sizeof(inc));
				parse_file(inc, depth + 1);
			}
		}
		else if (!strcmp(n->tag, "subset"))
		{
			// option group: the first <include> is the default choice
			const char *sname = attr_get(n, "name");
			if (!sname) continue;
			for (int j = 0; j < n->n_children; j++)
			{
				XMLNode *c = n->children[j];
				if (!c->tag || strcmp(c->tag, "include")) continue;
				const char *cname = attr_get(c, "name");
				if (!cname) continue;

				if (subset_cnt < TH_MAX_SUBSETS && !subset_chosen(sname))
				{
					snprintf(subsets[subset_cnt].name, sizeof(subsets[0].name), "%s", sname);
					snprintf(subsets[subset_cnt].chosen, sizeof(subsets[0].chosen), "%s", cname);
					subset_cnt++;
				}
				// parse the chosen option's include file (if it has one)
				const char *ch = subset_chosen(sname);
				if (ch && !strcasecmp(ch, cname))
				{
					char val[1024], inc[1024];
					node_text(c, val, sizeof(val));
					if (val[0])
					{
						resolve_path(val, base_dir, inc, sizeof(inc));
						parse_file(inc, depth + 1);
					}
				}
				break; // only the first include matters (it's the default)
			}
		}
		else if (!strcmp(n->tag, "variables"))
		{
			for (int j = 0; j < n->n_children; j++)
			{
				XMLNode *v = n->children[j];
				if (!v->tag || (v->tag_type != TAG_FATHER && v->tag_type != TAG_SELF)) continue;
				if (!strcmp(v->tag, "include")) continue; // stray option stubs
				if (!if_subset_ok(v)) continue;
				char val[512];
				node_text(v, val, sizeof(val));
				var_set(v->tag, val);
			}
		}
		else if (!strcmp(n->tag, "view"))
		{
			for (int j = 0; j < n->n_attributes; j++)
			{
				if (!strcmp(n->attributes[j].name, "name") &&
				    view_matches(n->attributes[j].value, want_view))
				{
					parse_view(n, base_dir);
					break;
				}
			}
		}
	}
	XMLDoc_free(&doc);
}

// ---------------------------------------------------------------------------
// Fonts (stb_truetype) + glyph cache
// ---------------------------------------------------------------------------

struct th_font_t
{
	char path[512];
	uint8_t *data;
	stbtt_fontinfo info;
	int ok;
};

#define TH_MAX_FONTS 4
static th_font_t fonts[TH_MAX_FONTS];
static int font_cnt = 0;

static int font_get(const char *path)
{
	if (!path || !path[0]) return -1;
	for (int i = 0; i < font_cnt; i++)
		if (!strcmp(fonts[i].path, path)) return fonts[i].ok ? i : -1;

	if (font_cnt >= TH_MAX_FONTS) return -1;
	th_font_t *f = &fonts[font_cnt];
	memset(f, 0, sizeof(*f));
	snprintf(f->path, sizeof(f->path), "%s", path);
	font_cnt++;

	FILE *fp = fopen(path, "rb");
	if (!fp) { printf("FBUI theme: no font %s\n", path); return -1; }
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	f->data = (uint8_t*)malloc((size_t)sz);
	if (!f->data || fread(f->data, 1, (size_t)sz, fp) != (size_t)sz)
	{
		fclose(fp);
		free(f->data);
		f->data = NULL;
		return -1;
	}
	fclose(fp);

	if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0)))
	{
		free(f->data);
		f->data = NULL;
		return -1;
	}
	f->ok = 1;
	printf("FBUI theme: font loaded: %s\n", path);
	return font_cnt - 1;
}

struct glyph_t
{
	uint32_t key;   // (font+1)<<28 | px<<20 | cp ; 0 = empty
	int w, h, xoff, yoff, adv;
	uint8_t *bmp;   // 8-bit alpha
};

#define GCACHE_SIZE 2048
static glyph_t gcache[GCACHE_SIZE];

static glyph_t *glyph_get(int font, int px, uint32_t cp)
{
	if (font < 0 || px < 4 || px > 255 || cp > 0xFFFFF) return NULL;
	th_font_t *f = &fonts[font];
	if (!f->ok) return NULL;

	uint32_t key = ((uint32_t)(font + 1) << 28) | ((uint32_t)px << 20) | cp;
	glyph_t *g = &gcache[key % GCACHE_SIZE];
	if (g->key == key) return g;

	int gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
	if (!gi) return NULL;

	free(g->bmp);
	memset(g, 0, sizeof(*g));

	float scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
	g->bmp = stbtt_GetGlyphBitmap(&f->info, scale, scale, gi, &g->w, &g->h, &g->xoff, &g->yoff);
	int adv, lsb;
	stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
	g->adv = (int)lroundf(adv * scale);
	g->key = key;
	return g;
}

static int font_ascent_px(int font, int px)
{
	th_font_t *f = &fonts[font];
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
	float scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
	return (int)lroundf(asc * scale);
}

// ---------------------------------------------------------------------------
// Pixel blending
// ---------------------------------------------------------------------------

static inline void px_blend(uint32_t *dst, uint32_t rgb, uint32_t a)
{
	if (!a) return;
	if (a >= 255) { *dst = rgb & 0xFFFFFF; return; }
	uint32_t d = *dst;
	uint32_t na = 255 - a;
	uint32_t r = (((rgb >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * na) / 255;
	uint32_t g = (((rgb >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * na) / 255;
	uint32_t b = ((rgb & 0xFF) * a + (d & 0xFF) * na) / 255;
	*dst = (r << 16) | (g << 8) | b;
}

// ---------------------------------------------------------------------------
// Text rendering (TTF + unifont fallback)
// ---------------------------------------------------------------------------

static int main_font_idx(void)
{
	return font_get(main_font);
}

// fallback: unifont glyph (16px design) scaled to px, top-aligned in line box
static int draw_fallback_glyph(uint32_t *fb, int fbw, int fbh, int x, int y,
                               uint32_t cp, uint32_t rgb, int px)
{
	const uint8_t *rec = fbui_hexfont_get(cp);
	int cell_w = rec ? (rec[0] ? 16 : 8) : (cp < 0x80 ? 8 : 16);
	int out_w = cell_w * px / 16;
	if (out_w < 1) out_w = 1;
	if (!rec) return out_w; // no bitmap: blank advance

	const uint8_t *bm = rec + 1;
	for (int oy = 0; oy < px; oy++)
	{
		int sy = oy * 16 / px;
		int fy = y + oy;
		if (fy < 0 || fy >= fbh) continue;
		for (int ox = 0; ox < out_w; ox++)
		{
			int sx = ox * cell_w / out_w;
			int fx = x + ox;
			if (fx < 0 || fx >= fbw) continue;
			int on = rec[0] ? (bm[sy * 2 + (sx >> 3)] >> (7 - (sx & 7))) & 1
			                : (bm[sy] >> (7 - (sx & 7))) & 1;
			if (on) px_blend(fb + fy * fbw + fx, rgb, 255);
		}
	}
	return out_w;
}

static int glyph_advance(int font, int px, uint32_t cp)
{
	glyph_t *g = glyph_get(font, px, cp);
	if (g) return g->adv;
	const uint8_t *rec = fbui_hexfont_get(cp);
	int cell_w = rec ? (rec[0] ? 16 : 8) : (cp < 0x80 ? 8 : 16);
	return cell_w * px / 16;
}

int theme_text_width(const char *utf8, int font_px)
{
	int font = bitmap_font ? -1 : main_font_idx();
	int w = 0;
	const char *p = utf8;
	while (*p)
	{
		uint32_t cp;
		p += utf8_decode(p, &cp);
		w += glyph_advance(font, font_px, cp);
	}
	return w;
}

int theme_draw_text(uint32_t *fb, int fbw, int fbh, int x, int y,
                    const char *utf8, uint32_t color, int font_px,
                    int max_w, int align)
{
	int font = bitmap_font ? -1 : main_font_idx();
	uint32_t rgb = color & 0xFFFFFF;

	if (max_w > 0 && align != THEME_ALIGN_LEFT)
	{
		int tw = theme_text_width(utf8, font_px);
		if (tw < max_w)
			x += (align == THEME_ALIGN_CENTER) ? (max_w - tw) / 2 : (max_w - tw);
	}

	int baseline = y + ((font >= 0) ? font_ascent_px(font, font_px) : font_px * 13 / 16);
	int cx = 0;
	const char *p = utf8;
	while (*p)
	{
		uint32_t cp;
		p += utf8_decode(p, &cp);
		int adv = glyph_advance(font, font_px, cp);
		if (max_w > 0 && cx + adv > max_w) break;

		glyph_t *g = glyph_get(font, font_px, cp);
		if (g)
		{
			for (int gy = 0; gy < g->h; gy++)
			{
				int fy = baseline + g->yoff + gy;
				if (fy < 0 || fy >= fbh) continue;
				const uint8_t *src = g->bmp + gy * g->w;
				for (int gx = 0; gx < g->w; gx++)
				{
					int fx = x + cx + g->xoff + gx;
					if (fx < 0 || fx >= fbw) continue;
					px_blend(fb + fy * fbw + fx, rgb, src[gx]);
				}
			}
		}
		else
		{
			draw_fallback_glyph(fb, fbw, fbh, x + cx, y, cp, rgb, font_px);
		}
		cx += adv;
	}
	return cx;
}

int theme_has_font(void)
{
	return bitmap_font || main_font_idx() >= 0;
}

// ---------------------------------------------------------------------------
// Images (SVG via nanosvg, PNG/JPG via Imlib2)
// ---------------------------------------------------------------------------

static int is_svg(const char *path)
{
	const char *dot = strrchr(path, '.');
	return dot && !strcasecmp(dot, ".svg");
}

enum { BLIT_FIT, BLIT_STRETCH, BLIT_TILE, BLIT_COVER };

// modulate a source pixel by the element tint (0xAARRGGBB, FFFFFFFF = none)
static inline uint32_t tint_apply(uint32_t argb, uint32_t tint, uint32_t *a_out)
{
	uint32_t a = argb >> 24;
	if (tint != 0xFFFFFFFF)
	{
		a = a * (tint >> 24) / 255;
		uint32_t r = ((argb >> 16) & 0xFF) * ((tint >> 16) & 0xFF) / 255;
		uint32_t g = ((argb >> 8) & 0xFF) * ((tint >> 8) & 0xFF) / 255;
		uint32_t b = (argb & 0xFF) * (tint & 0xFF) / 255;
		argb = (r << 16) | (g << 8) | b;
	}
	*a_out = a;
	return argb & 0xFFFFFF;
}

// blend a straight-alpha RGBA byte buffer into fb
static void blit_rgba(uint32_t *fb, int fbw, int fbh, const uint8_t *rgba,
                      int w, int h, int dx, int dy, uint32_t tint)
{
	for (int y = 0; y < h; y++)
	{
		int fy = dy + y;
		if (fy < 0 || fy >= fbh) continue;
		const uint8_t *src = rgba + (size_t)y * w * 4;
		for (int x = 0; x < w; x++)
		{
			int fx = dx + x;
			if (fx < 0 || fx >= fbw) continue;
			uint32_t argb = ((uint32_t)src[x * 4 + 3] << 24) | ((uint32_t)src[x * 4] << 16) |
			                ((uint32_t)src[x * 4 + 1] << 8) | src[x * 4 + 2];
			uint32_t a;
			uint32_t rgb = tint_apply(argb, tint, &a);
			px_blend(fb + fy * fbw + fx, rgb, a);
		}
	}
}

static int blit_svg(uint32_t *fb, int fbw, int fbh, const char *path,
                    int bx, int by, int bw, int bh, int mode, uint32_t tint)
{
	NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
	if (!img) return 0;
	if (img->width <= 0 || img->height <= 0) { nsvgDelete(img); return 0; }

	float sx = (float)bw / img->width;
	float sy = (float)bh / img->height;
	float scale;
	int w, h;
	float tx = 0, ty = 0;
	if (mode == BLIT_COVER)
	{
		scale = (sx > sy) ? sx : sy; // fill the box, crop overflow
		w = bw;
		h = bh;
		tx = (bw - img->width * scale) / 2;
		ty = (bh - img->height * scale) / 2;
	}
	else
	{
		scale = (sx < sy) ? sx : sy; // fit inside the box
		w = (int)(img->width * scale);
		h = (int)(img->height * scale);
	}
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	uint8_t *buf = (uint8_t*)malloc((size_t)w * h * 4);
	NSVGrasterizer *rast = nsvgCreateRasterizer();
	if (buf && rast)
	{
		nsvgRasterize(rast, img, tx, ty, scale, buf, w, h, w * 4);
		blit_rgba(fb, fbw, fbh, buf, w, h, bx + (bw - w) / 2, by + (bh - h) / 2, tint);
	}
	if (rast) nsvgDeleteRasterizer(rast);
	free(buf);
	nsvgDelete(img);
	return 1;
}

// blend an Imlib2 ARGB data buffer into fb
static void blit_argb(uint32_t *fb, int fbw, int fbh, const uint32_t *data,
                      int w, int h, int dx, int dy, int has_alpha, uint32_t tint)
{
	for (int y = 0; y < h; y++)
	{
		int fy = dy + y;
		if (fy < 0 || fy >= fbh) continue;
		for (int x = 0; x < w; x++)
		{
			int fx = dx + x;
			if (fx < 0 || fx >= fbw) continue;
			uint32_t argb = data[y * w + x];
			if (!has_alpha) argb |= 0xFF000000;
			uint32_t a;
			uint32_t rgb = tint_apply(argb, tint, &a);
			px_blend(fb + fy * fbw + fx, rgb, a);
		}
	}
}

static int blit_raster(uint32_t *fb, int fbw, int fbh, const char *path,
                       int bx, int by, int bw, int bh, int mode, uint32_t tint)
{
	Imlib_Image img = imlib_load_image(path);
	if (!img) return 0;
	imlib_context_set_image(img);
	int sw = imlib_image_get_width();
	int sh = imlib_image_get_height();
	int has_alpha = imlib_image_has_alpha();
	if (sw <= 0 || sh <= 0) { imlib_free_image(); return 0; }

	if (mode == BLIT_TILE)
	{
		uint32_t *data = imlib_image_get_data_for_reading_only();
		if (data)
		{
			for (int ty = by; ty < by + bh; ty += sh)
			{
				for (int tx = bx; tx < bx + bw; tx += sw)
				{
					int cw = (tx + sw > bx + bw) ? (bx + bw - tx) : sw;
					int ch = (ty + sh > by + bh) ? (by + bh - ty) : sh;
					for (int y = 0; y < ch; y++)
					{
						int fy = ty + y;
						if (fy < 0 || fy >= fbh) continue;
						for (int x = 0; x < cw; x++)
						{
							int fx = tx + x;
							if (fx < 0 || fx >= fbw) continue;
							uint32_t argb = data[y * sw + x];
							if (!has_alpha) argb |= 0xFF000000;
							uint32_t a;
							uint32_t rgb = tint_apply(argb, tint, &a);
							px_blend(fb + fy * fbw + fx, rgb, a);
						}
					}
				}
			}
		}
		imlib_free_image();
		return 1;
	}

	int dw, dh;
	int cx = 0, cy = 0, cw = sw, ch = sh;
	if (mode == BLIT_STRETCH)
	{
		dw = bw;
		dh = bh;
	}
	else if (mode == BLIT_COVER)
	{
		// crop the source to the box aspect ratio, then fill the box
		if ((int64_t)sw * bh > (int64_t)sh * bw)
		{
			cw = (int)((int64_t)sh * bw / bh);
			cx = (sw - cw) / 2;
		}
		else
		{
			ch = (int)((int64_t)sw * bh / bw);
			cy = (sh - ch) / 2;
		}
		if (cw < 1) cw = 1;
		if (ch < 1) ch = 1;
		dw = bw;
		dh = bh;
	}
	else
	{
		dw = bw; dh = sh * bw / sw;
		if (dh > bh) { dh = bh; dw = sw * bh / sh; }
	}
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;

	Imlib_Image scaled = imlib_create_cropped_scaled_image(cx, cy, cw, ch, dw, dh);
	imlib_free_image(); // original
	if (!scaled) return 0;

	imlib_context_set_image(scaled);
	imlib_image_set_has_alpha(has_alpha); // scaling may drop the flag
	uint32_t *data = imlib_image_get_data_for_reading_only();
	if (data)
	{
		blit_argb(fb, fbw, fbh, data, dw, dh,
			bx + (bw - dw) / 2, by + (bh - dh) / 2, has_alpha, tint);
	}
	imlib_free_image(); // scaled
	return 1;
}

static int blit_img(uint32_t *fb, int fbw, int fbh, const char *path,
                    int bx, int by, int bw, int bh, int mode, uint32_t tint)
{
	if (!path[0] || bw <= 0 || bh <= 0) return 0;
	if (access(path, R_OK)) return 0;
	return is_svg(path) ? blit_svg(fb, fbw, fbh, path, bx, by, bw, bh, mode, tint)
	                    : blit_raster(fb, fbw, fbh, path, bx, by, bw, bh, mode, tint);
}

int theme_blit_image_file(uint32_t *fb, int fbw, int fbh, const char *path,
                          int bx, int by, int bw, int bh)
{
	return blit_img(fb, fbw, fbh, path, bx, by, bw, bh, BLIT_FIT, 0xFFFFFFFF);
}

int theme_blit_image_cover(uint32_t *fb, int fbw, int fbh, const char *path,
                           int bx, int by, int bw, int bh)
{
	return blit_img(fb, fbw, fbh, path, bx, by, bw, bh, BLIT_COVER, 0xFFFFFFFF);
}

int theme_blit_image_cover_tint(uint32_t *fb, int fbw, int fbh, const char *path,
                                int bx, int by, int bw, int bh, uint32_t tint)
{
	return blit_img(fb, fbw, fbh, path, bx, by, bw, bh, BLIT_COVER, tint);
}

uint32_t *theme_load_cover_rgb(const char *path, int max_w, int max_h,
                               uint32_t tint, int *out_w, int *out_h)
{
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!path || !path[0] || max_w < 8 || max_h < 8) return NULL;
	if (access(path, R_OK)) return NULL;

	uint32_t *buf = (uint32_t*)malloc((size_t)max_w * max_h * 4);
	if (!buf) return NULL;
	memset(buf, 0, (size_t)max_w * max_h * 4);
	if (!blit_img(buf, max_w, max_h, path, 0, 0, max_w, max_h, BLIT_COVER, tint))
	{
		free(buf);
		return NULL;
	}
	if (out_w) *out_w = max_w;
	if (out_h) *out_h = max_h;
	return buf;
}

uint32_t *theme_load_fit_rgb(const char *path, int max_w, int max_h,
                             int *out_w, int *out_h)
{
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!path || !path[0] || max_w < 8 || max_h < 8) return NULL;
	if (access(path, R_OK)) return NULL;

	// Fit into max box: render into max buffer then we keep max dims for simple blit-up.
	uint32_t *buf = (uint32_t*)calloc((size_t)max_w * max_h, 4);
	if (!buf) return NULL;
	if (!blit_img(buf, max_w, max_h, path, 0, 0, max_w, max_h, BLIT_FIT, 0xFFFFFFFF))
	{
		free(buf);
		return NULL;
	}
	if (out_w) *out_w = max_w;
	if (out_h) *out_h = max_h;
	return buf;
}

// ---------------------------------------------------------------------------
// Element geometry
// ---------------------------------------------------------------------------

static void el_rect(const th_el_t *e, int *x, int *y, int *w, int *h)
{
	float fw = e->size_w, fh = e->size_h;
	if (fw <= 0 && e->maxs_w > 0) fw = e->maxs_w;
	if (fh <= 0 && e->maxs_h > 0) fh = e->maxs_h;
	if (fw <= 0 && e->mins_w > 0) fw = e->mins_w;
	if (fh <= 0 && e->mins_h > 0) fh = e->mins_h;
	if (fw <= 0) fw = 0.2f;
	if (fh <= 0) fh = 0.2f;

	*w = (int)(fw * scrW);
	*h = (int)(fh * scrH);
	*x = (int)(e->pos_x * scrW - e->org_x * (*w));
	*y = (int)(e->pos_y * scrH - e->org_y * (*h));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int theme_load(const char *xml_path, const char *view,
               const char *sys_name, const char *sys_theme,
               int scr_w, int scr_h)
{
	theme_unload();
	scrW = scr_w;
	scrH = scr_h;
	snprintf(want_view, sizeof(want_view), "%s", view);

	var_set("system.name", sys_name ? sys_name : "");
	var_set("system.theme", sys_theme ? sys_theme : "");
	var_set("system.fullName", sys_name ? sys_name : "");

	char troot[512];
	snprintf(troot, sizeof(troot), "%s", xml_path);
	char *sl = strrchr(troot, '/');
	if (sl) *sl = 0;
	var_set("themePath", troot);

	parse_file(xml_path, 0);

	// "logoText" is the fallback shown when the "logo" image is missing
	for (int i = 0; i < el_cnt; i++)
	{
		if (els[i].type == EL_IMAGE && !strcmp(els[i].name, "logo") && els[i].path[0])
		{
			for (int j = 0; j < el_cnt; j++)
				if (els[j].type == EL_TEXT && !strcmp(els[j].name, "logoText"))
					els[j].visible = 0;
			break;
		}
	}

	// hide metadata captions ("X_label") whose value ("X_text") is bound to
	// game data we can't provide - a bare "Developer"/"Last Played" caption
	// with nothing under it is just clutter
	for (int i = 0; i < el_cnt; i++)
	{
		if (els[i].type != EL_TEXT) continue;
		size_t nl = strlen(els[i].name);
		if (nl <= 6 || strcmp(els[i].name + nl - 6, "_label")) continue;

		char partner[64];
		snprintf(partner, sizeof(partner), "%.*s_text", (int)(nl - 6), els[i].name);
		int ok = 0;
		for (int j = 0; j < el_cnt; j++)
		{
			if (els[j].type == EL_TEXT && !strcmp(els[j].name, partner) &&
			    !els[j].dynamic && els[j].visible && els[j].text[0])
			{
				ok = 1;
				break;
			}
		}
		if (!ok) els[i].visible = 0;
	}

	// remember the list font (or any text font) for theme_draw_text
	main_font[0] = 0;
	for (int i = 0; i < el_cnt; i++)
	{
		if (els[i].type == EL_TEXTLIST && els[i].font_path[0])
		{
			snprintf(main_font, sizeof(main_font), "%s", els[i].font_path);
			break;
		}
	}
	if (!main_font[0])
	{
		for (int i = 0; i < el_cnt; i++)
		{
			if (els[i].type == EL_TEXT && els[i].font_path[0])
			{
				snprintf(main_font, sizeof(main_font), "%s", els[i].font_path);
				break;
			}
		}
	}

	th_loaded = (el_cnt > 0);
	printf("FBUI theme: %s view '%s': %d elements, font '%s'\n",
		xml_path, view, el_cnt, main_font);
	return el_cnt;
}

void theme_unload(void)
{
	el_cnt = 0;
	var_cnt = 0;
	subset_cnt = 0;
	th_loaded = 0;
	main_font[0] = 0;
	bitmap_font = 0;
}

int theme_active(void)
{
	return th_loaded;
}

void theme_set_bitmap_font(int enable)
{
	bitmap_font = !!enable;
	printf("FBUI theme font: %s\n", bitmap_font ? "240p bitmap" : "HD TrueType");
}

void theme_render_static(uint32_t *fb, int fbw, int fbh)
{
	// draw in zIndex order (stable insertion sort on small array)
	int order[TH_MAX_EL];
	for (int i = 0; i < el_cnt; i++) order[i] = i;
	for (int i = 1; i < el_cnt; i++)
	{
		int k = order[i], j = i - 1;
		while (j >= 0 && els[order[j]].z > els[k].z) { order[j + 1] = order[j]; j--; }
		order[j + 1] = k;
	}

	for (int i = 0; i < fbw * fbh; i++) fb[i] = 0xFF101010;

	for (int i = 0; i < el_cnt; i++)
	{
		th_el_t *e = &els[order[i]];
		if (!e->visible || e->dynamic) continue;
		if (!strncmp(e->name, "md_", 3)) continue; // metadata slots: dynamic
		if (el_hidden(e->name)) continue;

		int x, y, w, h;
		el_rect(e, &x, &y, &w, &h);

		if (e->type == EL_IMAGE && e->path[0])
		{
			int mode = e->tile ? BLIT_TILE :
			           e->has_min ? BLIT_COVER :
			           (e->has_size && e->size_w > 0 && e->size_h > 0) ? BLIT_STRETCH : BLIT_FIT;
			blit_img(fb, fbw, fbh, e->path, x, y, w, h, mode, e->tint);
		}
		else if (e->type == EL_TEXT && e->text[0])
		{
			int px = (int)(e->font_size * scrH);
			if (px < 8) px = 8;
			theme_draw_text(fb, fbw, fbh, x, y, e->text, e->color, px, w, e->align);
		}
	}
}

int theme_get_list(theme_list_style_t *out)
{
	for (int i = 0; i < el_cnt; i++)
	{
		th_el_t *e = &els[i];
		if (e->type != EL_TEXTLIST) continue;

		el_rect(e, &out->x, &out->y, &out->w, &out->h);
		out->font_px = (int)(e->font_size * scrH);
		if (out->font_px < 8) out->font_px = 8;
		out->row_h = (int)(out->font_px * e->line_spacing);
		if (out->row_h < out->font_px + 2) out->row_h = out->font_px + 2;
		out->align = e->align;
		// selector bars often carry alpha (e.g. FFFFFF40); FBUI fills opaque
		// rects, so pre-blend towards black
		uint32_t a = e->sel_bar >> 24;
		out->sel_bar = ((((e->sel_bar >> 16) & 0xFF) * a / 255) << 16) |
		               ((((e->sel_bar >> 8) & 0xFF) * a / 255) << 8) |
		               (((e->sel_bar & 0xFF) * a / 255));
		out->sel_text = e->sel_text;
		out->primary = e->color;
		out->secondary = e->secondary;
		return 1;
	}
	return 0;
}

int theme_get_image_rect(const char *name, int *x, int *y, int *w, int *h)
{
	for (int i = 0; i < el_cnt; i++)
	{
		if (els[i].type == EL_IMAGE && els[i].visible && !strcmp(els[i].name, name))
		{
			el_rect(&els[i], x, y, w, h);
			return 1;
		}
	}
	// themes that hide md_image usually declare their own game art slot
	// bound to {game:image}/{game:thumbnail}. Full-screen slots are
	// background screenshots, not cover boxes: prefer the smallest one.
	if (!strcmp(name, "md_image"))
	{
		int best = -1;
		int64_t best_area = 0;
		for (int i = 0; i < el_cnt; i++)
		{
			if (els[i].type != EL_IMAGE || !els[i].is_gameart || !els[i].visible) continue;
			int ex, ey, ew, eh;
			el_rect(&els[i], &ex, &ey, &ew, &eh);
			if (ew <= 0 || eh <= 0) continue;
			if (ew >= scrW * 9 / 10 && eh >= scrH * 9 / 10) continue; // background
			int64_t area = (int64_t)ew * eh;
			if (best < 0 || area < best_area)
			{
				best = i;
				best_area = area;
			}
		}
		if (best >= 0)
		{
			el_rect(&els[best], x, y, w, h);
			return 1;
		}
	}
	return 0;
}

int theme_get_help(int *x, int *y, uint32_t *color, int *font_px)
{
	for (int i = 0; i < el_cnt; i++)
	{
		th_el_t *e = &els[i];
		if (e->type != EL_HELP) continue;
		*x = (int)(e->pos_x * scrW);
		*y = (int)(e->pos_y * scrH);
		*color = e->color;
		*font_px = (int)(0.035f * scrH);
		return 1;
	}
	return 0;
}
