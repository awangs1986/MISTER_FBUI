// FBUI TrueType 文字引擎。
//
// 只服务 HD 档：用 stb_truetype 排版皮肤指定的 TTF，并缓存
// (字体, 字号, 码点) -> 8bpp alpha 位图；TTF 缺失的码点（主要是 CJK）
// 回退到 GNU Unifont 的点阵字形。
//
// 240p 档完全不经过这里 —— 那一档由 fbui.cpp 里的位图引擎直接画，
// 因为 16px 的抗锯齿字面在 240 线 CRT 上反而不如点阵清楚。

#ifndef FBUI_TEXT_H
#define FBUI_TEXT_H

#include <stdint.h>

// align 取值与 fbui_skin.h 的 SKIN_ALIGN_* 一致：0 左 / 1 中 / 2 右。

// 指定 TrueType 文件。传 NULL 或空串表示清除（之后一律走点阵回退）。
void fbui_ttf_set_font(const char *ttf_path);

// 1 表示当前有可用的 TrueType 字面。
int  fbui_ttf_available(void);

// 释放已光栅化的字形位图（1080p 下几 MiB）。
void fbui_ttf_cache_clear(void);

// 画 UTF-8 文本。x/y 是行框左上角；max_w > 0 时按 align 在 [x, x+max_w]
// 内对齐并裁剪。color 为 0xRRGGBB。返回实际绘制宽度。
int  fbui_ttf_draw(uint32_t *fb, int fbw, int fbh, int x, int y,
                   const char *utf8, uint32_t color, int font_px,
                   int max_w, int align);

int  fbui_ttf_width(const char *utf8, int font_px);

#endif // FBUI_TEXT_H
