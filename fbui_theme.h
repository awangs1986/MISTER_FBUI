// FBUI theme engine — parses a subset of EmulationStation / Batocera
// theme.xml files and renders their elements into the FBUI shadow buffer.
//
// Supported subset:
//   <include>, <variables>/${var}, <view name="detailed|basic|...">
//   elements: image, text, textlist, helpsystem
//   properties: pos, size, maxSize, origin, zIndex, visible, path, text,
//               color, fontPath, fontSize, alignment, lineSpacing,
//               selectorColor, selectedColor, primaryColor, secondaryColor,
//               textColor
// Unsupported (skipped): video, carousel, ninepatch, rating, datetime, sound.

#ifndef FBUI_THEME_H
#define FBUI_THEME_H

#include <stdint.h>

#define THEME_ALIGN_LEFT   0
#define THEME_ALIGN_CENTER 1
#define THEME_ALIGN_RIGHT  2

struct theme_list_style_t
{
	int x, y, w, h;      // textlist rect in pixels
	int row_h;           // row height (font_px * lineSpacing)
	int font_px;         // font pixel height
	int align;
	uint32_t sel_bar;    // selectorColor  (bar behind selected row)
	uint32_t sel_text;   // selectedColor  (selected row text)
	uint32_t primary;    // primaryColor   (normal entries)
	uint32_t secondary;  // secondaryColor (folders)
};

// Parse <xml_path> collecting elements of <view> ("detailed"/"basic"/...).
// sys_name/sys_theme fill the ${system.name}/${system.theme} variables.
// Returns number of elements collected (0 = view empty or parse failed).
int  theme_load(const char *xml_path, const char *view,
                const char *sys_name, const char *sys_theme,
                int scr_w, int scr_h);
void theme_unload(void);
int  theme_active(void);

// Draw all static elements (z-sorted images and fixed text; metadata
// placeholders named md_* are skipped) into an 0RGB32 buffer.
void theme_render_static(uint32_t *fb, int fbw, int fbh);

// Additionally skip elements with these names (comma separated, e.g.
// "logo,logo2" when the loaded per-system theme is only borrowed for
// styling). Pass "" to reset.
void theme_set_hidden(const char *names_csv);

// Textlist style of the loaded view. Returns 1 if the view has a textlist.
int  theme_get_list(theme_list_style_t *out);

// Pixel rect of a named image element (e.g. "md_image"). Returns 1 if found.
int  theme_get_image_rect(const char *name, int *x, int *y, int *w, int *h);

// helpsystem position/color. Returns 1 if the view defines one.
int  theme_get_help(int *x, int *y, uint32_t *color, int *font_px);

// 1 when a usable TTF font came with the theme (theme_draw_text will not
// just fall back to the bitmap font).
int  theme_has_font(void);

// Draw UTF-8 text with the theme TTF (unifont/builtin fallback for glyphs
// the TTF lacks, e.g. CJK). x/y = top-left of the line box; when max_w > 0
// the text is clipped and align is applied inside [x, x+max_w].
// color = 0xRRGGBB. Returns drawn width in px.
int  theme_draw_text(uint32_t *fb, int fbw, int fbh, int x, int y,
                     const char *utf8, uint32_t color, int font_px,
                     int max_w, int align);
int  theme_text_width(const char *utf8, int font_px);

// Load svg/png/jpg, scale to fit inside the box (aspect kept), alpha-blend
// centered. Returns 1 when something was drawn.
int  theme_blit_image_file(uint32_t *fb, int fbw, int fbh, const char *path,
                           int bx, int by, int bw, int bh);
// Same, but fill the box (crop overflow) — for full-bleed backgrounds.
int  theme_blit_image_cover(uint32_t *fb, int fbw, int fbh, const char *path,
                            int bx, int by, int bw, int bh);
int  theme_blit_image_cover_tint(uint32_t *fb, int fbw, int fbh, const char *path,
                                 int bx, int by, int bw, int bh, uint32_t tint);

// Decode once into malloc'd 0RGB32 (max_w×max_h). Caller frees. NULL on fail.
uint32_t *theme_load_cover_rgb(const char *path, int max_w, int max_h,
                               uint32_t tint, int *out_w, int *out_h);
uint32_t *theme_load_fit_rgb(const char *path, int max_w, int max_h,
                             int *out_w, int *out_h);

#endif // FBUI_THEME_H
