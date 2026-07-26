// FBUI TrueType 文字引擎实现，见 fbui_text.h。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "fbui.h"
#include "fbui_text.h"
#include "font_cjk.h"   // utf8_decode

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "lib/stb/stb_truetype.h"
#pragma GCC diagnostic pop

#define ALIGN_LEFT   0
#define ALIGN_CENTER 1
#define ALIGN_RIGHT  2

// ---------------------------------------------------------------------------
// 字体
// ---------------------------------------------------------------------------

struct ttf_font_t
{
	char path[512];
	uint8_t *data;
	stbtt_fontinfo info;
	int ok;
};

#define MAX_FONTS 4
static ttf_font_t fonts[MAX_FONTS];
static int font_cnt = 0;
static char main_font[512];

static int font_get(const char *path)
{
	if (!path || !path[0]) return -1;
	for (int i = 0; i < font_cnt; i++)
		if (!strcmp(fonts[i].path, path)) return fonts[i].ok ? i : -1;

	if (font_cnt >= MAX_FONTS) return -1;
	ttf_font_t *f = &fonts[font_cnt];
	memset(f, 0, sizeof(*f));
	snprintf(f->path, sizeof(f->path), "%s", path);
	font_cnt++;

	FILE *fp = fopen(path, "rb");
	if (!fp) { printf("FBUI text: no font %s\n", path); return -1; }
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
	printf("FBUI text: font loaded: %s\n", path);
	return font_cnt - 1;
}

static int main_font_idx(void)
{
	return font_get(main_font);
}

void fbui_ttf_set_font(const char *ttf_path)
{
	if (ttf_path && ttf_path[0]) snprintf(main_font, sizeof(main_font), "%s", ttf_path);
	else main_font[0] = 0;
}

int fbui_ttf_available(void)
{
	return main_font_idx() >= 0;
}

// ---------------------------------------------------------------------------
// 字形缓存
// ---------------------------------------------------------------------------

struct glyph_t
{
	uint32_t key;   // (font+1)<<28 | px<<20 | cp ; 0 = 空
	int w, h, xoff, yoff, adv;
	uint8_t *bmp;   // 8bpp alpha
};

#define GCACHE_SIZE 2048
static glyph_t gcache[GCACHE_SIZE];
static size_t gcache_bytes = 0;

// 原始 key 必须先打散再取模。字号位于第 20 位，而 2^20 是 2048 的整数倍，
// 直接取模会让同一个码点在两种字号下落到同一个槽 —— 标题和列表行就会
// 每帧互相踢出对方。
static inline uint32_t glyph_slot(uint32_t key)
{
	uint32_t h = key * 2654435761u;
	return (h >> 13) % GCACHE_SIZE;
}

static glyph_t *glyph_get(int font, int px, uint32_t cp)
{
	if (font < 0 || px < 4 || px > 255 || cp > 0xFFFFF) return NULL;
	ttf_font_t *f = &fonts[font];
	if (!f->ok) return NULL;

	uint32_t key = ((uint32_t)(font + 1) << 28) | ((uint32_t)px << 20) | cp;
	glyph_t *g = &gcache[glyph_slot(key)];
	if (g->key == key) return g;

	int gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
	if (!gi) return NULL;

	if (g->bmp) gcache_bytes -= (size_t)g->w * g->h;
	free(g->bmp);
	memset(g, 0, sizeof(*g));

	float scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
	g->bmp = stbtt_GetGlyphBitmap(&f->info, scale, scale, gi, &g->w, &g->h, &g->xoff, &g->yoff);
	if (g->bmp) gcache_bytes += (size_t)g->w * g->h;
	int adv, lsb;
	stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
	g->adv = (int)lroundf(adv * scale);
	g->key = key;
	return g;
}

void fbui_ttf_cache_clear(void)
{
	if (gcache_bytes)
		printf("FBUI text: glyph cache freed (%.1f KiB)\n", gcache_bytes / 1024.0);
	for (int i = 0; i < GCACHE_SIZE; i++)
	{
		free(gcache[i].bmp);
		memset(&gcache[i], 0, sizeof(gcache[i]));
	}
	gcache_bytes = 0;
}

static int font_ascent_px(int font, int px)
{
	ttf_font_t *f = &fonts[font];
	int asc, desc, gap;
	stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
	float scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
	return (int)lroundf(asc * scale);
}

// ---------------------------------------------------------------------------
// 绘制
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

// TTF 里没有的码点：用 Unifont 的 16px 设计尺寸缩到 px，顶对齐在行框内。
static int draw_fallback_glyph(uint32_t *fb, int fbw, int fbh, int x, int y,
                               uint32_t cp, uint32_t rgb, int px)
{
	const uint8_t *rec = fbui_hexfont_get(cp);
	int cell_w = rec ? (rec[0] ? 16 : 8) : (cp < 0x80 ? 8 : 16);
	int out_w = cell_w * px / 16;
	if (out_w < 1) out_w = 1;
	if (!rec) return out_w; // 没有点阵：只留空位

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

// 已经取到字形时的步进，避免绘制循环里对同一个字符查两次缓存。
static int glyph_adv_for(glyph_t *g, int px, uint32_t cp)
{
	if (g) return g->adv;
	const uint8_t *rec = fbui_hexfont_get(cp);
	int cell_w = rec ? (rec[0] ? 16 : 8) : (cp < 0x80 ? 8 : 16);
	return cell_w * px / 16;
}

int fbui_ttf_width(const char *utf8, int font_px)
{
	int font = main_font_idx();
	int w = 0;
	const char *p = utf8;
	while (*p)
	{
		uint32_t cp;
		p += utf8_decode(p, &cp);
		w += glyph_adv_for(glyph_get(font, font_px, cp), font_px, cp);
	}
	return w;
}

int fbui_ttf_draw(uint32_t *fb, int fbw, int fbh, int x, int y,
                  const char *utf8, uint32_t color, int font_px,
                  int max_w, int align)
{
	int font = main_font_idx();
	uint32_t rgb = color & 0xFFFFFF;

	if (max_w > 0 && align != ALIGN_LEFT)
	{
		int tw = fbui_ttf_width(utf8, font_px);
		if (tw < max_w)
			x += (align == ALIGN_CENTER) ? (max_w - tw) / 2 : (max_w - tw);
	}

	int baseline = y + ((font >= 0) ? font_ascent_px(font, font_px) : font_px * 13 / 16);
	int cx = 0;
	const char *p = utf8;
	while (*p)
	{
		uint32_t cp;
		p += utf8_decode(p, &cp);
		glyph_t *g = glyph_get(font, font_px, cp);
		int adv = glyph_adv_for(g, font_px, cp);
		if (max_w > 0 && cx + adv > max_w) break;

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
