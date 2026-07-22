#include "font_cjk.h"

#include <stddef.h>

// Minimal demo glyphs for: 忍者猫日文版
// Native Zpix 12x12, column-major, low 12 bits per column (bit0=top).

struct cjk_glyph_t
{
	uint32_t cp;
	uint16_t cols[FONT_CJK_WIDTH];
};

static const cjk_glyph_t cjk_glyphs[] = {
	// Native Zpix 12x12, column-major, low 12 bits per column (bit0=top).
	{ 0x5FCD, { 0x000, 0x401, 0x325, 0x025, 0x795, 0x40F, 0x449, 0x489, 0x421, 0x621, 0x13F, 0x600 } },
	{ 0x8005, { 0x000, 0x090, 0x092, 0x7D2, 0x552, 0x572, 0x55F, 0x552, 0x552, 0x558, 0x7D4, 0x012 } },
	{ 0x732B, { 0x000, 0x245, 0x422, 0x3FD, 0x000, 0x7F2, 0x497, 0x492, 0x7F2, 0x492, 0x497, 0x7F2 } },
	{ 0x65E5, { 0x000, 0x000, 0x7FF, 0x421, 0x421, 0x421, 0x421, 0x421, 0x421, 0x421, 0x7FF, 0x000 } },
	{ 0x6587, { 0x000, 0x404, 0x404, 0x21C, 0x224, 0x144, 0x087, 0x144, 0x224, 0x21C, 0x404, 0x404 } },
	{ 0x7248, { 0x000, 0x400, 0x3FF, 0x048, 0x7CF, 0x008, 0x600, 0x1FF, 0x019, 0x669, 0x189, 0x679 } },
};

const uint16_t *font_cjk_get(uint32_t codepoint)
{
	for (size_t i = 0; i < sizeof(cjk_glyphs) / sizeof(cjk_glyphs[0]); i++)
	{
		if (cjk_glyphs[i].cp == codepoint) return cjk_glyphs[i].cols;
	}
	return NULL;
}

int utf8_decode(const char *s, uint32_t *cp)
{
	const unsigned char *u = (const unsigned char *)s;
	unsigned char c0 = u[0];

	if (c0 < 0x80)
	{
		*cp = c0;
		return 1;
	}

	// 2-byte: 110xxxxx 10xxxxxx
	if ((c0 & 0xE0) == 0xC0)
	{
		unsigned char c1 = u[1];
		if (c0 >= 0xC2 && (c1 & 0xC0) == 0x80)
		{
			*cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
			return 2;
		}
	}
	// 3-byte: 1110xxxx 10xxxxxx 10xxxxxx  (CJK mainly here)
	else if ((c0 & 0xF0) == 0xE0)
	{
		unsigned char c1 = u[1];
		unsigned char c2 = u[2];
		if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80)
		{
			uint32_t v = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
			// reject overlong / surrogates
			if (v >= 0x800 && !(v >= 0xD800 && v <= 0xDFFF))
			{
				*cp = v;
				return 3;
			}
		}
	}
	// 4-byte: 11110xxx ...
	else if ((c0 & 0xF8) == 0xF0)
	{
		unsigned char c1 = u[1];
		unsigned char c2 = u[2];
		unsigned char c3 = u[3];
		if (c0 <= 0xF4 && (c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80)
		{
			uint32_t v = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
			if (v >= 0x10000 && v <= 0x10FFFF)
			{
				*cp = v;
				return 4;
			}
		}
	}

	// Invalid / legacy single-byte OSD icon (0x80+)
	*cp = c0;
	return 1;
}

int utf8_disp_cells(uint32_t cp)
{
	if (cp == 0x0B || cp == 0x0C || cp == 0x0A || cp == 0x0D) return 0;
	if (cp < 0x80) return 1;
	// 12px ≈ 2 OSD cells
	return 2;
}
