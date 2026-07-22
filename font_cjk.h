#ifndef FONT_CJK_H
#define FONT_CJK_H

#include <stdint.h>

// Native Zpix glyph size (pixels). Drawn in a 16px (2 OSD row) tall cell.
#define FONT_CJK_WIDTH 12
#define FONT_CJK_HEIGHT 12
// Vertical pad when placing 12px glyph into 16px (2+12+2).
#define FONT_CJK_PAD_TOP 2

// Returns 12 column words (bits 0..11 = top..bottom), or NULL if missing.
const uint16_t *font_cjk_get(uint32_t codepoint);

// Decode one UTF-8 sequence.
// Returns bytes consumed (1..4). On invalid sequence returns 1 and sets *cp to the raw byte.
int utf8_decode(const char *s, uint32_t *cp);

// Display width in OSD cells (8px): ASCII=1, CJK/unknown wide=2, controls=0.
int utf8_disp_cells(uint32_t cp);

#endif
