// FBUI 皮肤引擎 —— 以预烘焙 PNG 为主的皮肤包格式。
//
// 设计要点：
//   - 坐标按分辨率写死在 layout.ini 里，运行时不做任何缩放换算；
//   - 一次开机只有一种分辨率是活的（由开机 OSD 分辨率选择决定），
//     所以同一时刻只加载一个子目录，两套资源不会同时驻留；
//   - 皮肤包缺失时退回到内建默认值，FBUI 仍可正常工作。
//
// 皮肤包目录结构：
//   skins/<name>/
//     skin.ini              全局：显示名、字体
//     240p/                 640x240 专用资源
//       layout.ini          绝对像素坐标 + 颜色
//       bg.png              整屏已合成的静态层
//       systems/<es>.png    系统 logo
//       sysbg/<es>.png      系统背景
//     1080p/                1920x1080 专用资源
//       ...

#ifndef FBUI_SKIN_H
#define FBUI_SKIN_H

#include <stdint.h>
#include <stddef.h>

#define SKIN_ALIGN_LEFT   0
#define SKIN_ALIGN_CENTER 1
#define SKIN_ALIGN_RIGHT  2

struct skin_t
{
	int valid;              // 1 = 磁盘上真的加载到了皮肤包（0 = 用内建默认值）
	char dir[1024];         // 已解析的分辨率子目录，资源都在这里面
	char name[64];          // 显示名
	char font[512];         // TrueType 字体绝对路径，空 = 用内建位图字体
	int showcase;           // 1 = 系统层用整屏美术的 showcase 版式，0 = 普通列表
	int scr_w, scr_h;
	int lowres;             // 1 = 240p 档（scr_h <= 288）
	int scale;              // HD 档的整数放大系数（1080p 为 2）

	// 显示像素宽高比。640x240 在 4:3 屏上是 1:2（像素宽度只有高度的一半），
	// 所有需要保持观感比例的资源都要按这个做非等比预变形。
	int par_num, par_den;

	// CRT 过扫描安全区内缩
	int safe_l, safe_t, safe_r, safe_b;

	// 上下条带
	int header_h, footer_h;

	// 列表。list_rows 恒由高度除以行高反推，layout.ini 只给高度，
	// 免得皮肤作者手算行数算错导致最后一行被切掉。
	int list_x, list_y, list_w, list_rows;
	int list_h_override;    // layout.ini 的 list= 第四个分量，0 = 用安全区推算
	int row_h, row_pad, list_align, scrollbar_w;

	// 媒体面板。只有一个矩形：静态层是预烘焙的，位置若随浏览层级变动就会和
	// 烘焙好的底图对不上。想要更大的封面框，直接在 layout.ini 里把它画大。
	int panel_x, panel_y, panel_w, panel_h;

	// showcase（系统视图）的 logo 框
	int logo_x, logo_y, logo_w, logo_h;

	// 文字高度
	int font_px, title_px, footer_px;

	// 固定尺寸资源（帧缓冲像素，已经做过 PAR 预变形）
	int thumb_w, thumb_h;
	int sysbg_w, sysbg_h;
	int syslogo_w, syslogo_h;

	// 美术缓存总预算（MiB），showcase 与缩略图共用
	int cache_mb;

	// 颜色 0xRRGGBB
	uint32_t col_bg_top, col_bg_bot, col_header, col_panel;
	uint32_t col_text, col_dim, col_sel_bar, col_sel_text, col_accent;
	uint32_t col_title, col_msg;
};

// 按 scr_h 选择分辨率子目录并解析 layout.ini。name 为空或包不存在时
// 填入内建默认值并返回 0；成功加载磁盘皮肤包返回 1。
int skin_load(const char *name, int scr_w, int scr_h);
void skin_unload(void);

// 永远返回可用的皮肤描述（加载失败时是内建默认值）。
const skin_t *skin(void);

// 把 rel（如 "systems/nes.png"）拼成皮肤包内的完整路径。
// 文件可读返回 1，否则 out 置空并返回 0。
int skin_asset_path(const char *rel, char *out, size_t sz);

// 预合成静态层：解码 bg.png 一次并缓存，之后每次调用只做 memcpy。
// 皮肤包没有 bg.png 时返回 0，调用方应退回到程序内画的渐变。
int skin_fill_background(uint32_t *dst, int w, int h);

// 图像解码。统一返回 ARGB8888（未预乘），由调用方 free。
uint32_t *skin_img_load(const char *path, int *w, int *h);
// 缩放到恰好 w x h（允许非等比，用于已知固定尺寸的槽位）。
uint32_t *skin_img_load_exact(const char *path, int w, int h);
// 等比缩放到不超过 max_w x max_h。
uint32_t *skin_img_load_fit(const char *path, int max_w, int max_h, int *w, int *h);
// 按显示像素宽高比等比缩放到不超过 max_w x max_h，即视觉上不变形。
uint32_t *skin_img_load_fit_par(const char *path, int max_w, int max_h, int *w, int *h);
// 中心裁剪后铺满 w x h，可选按 0xAARRGGBB 做乘法着色（0xFFFFFFFF = 不变）。
uint32_t *skin_img_load_cover(const char *path, int w, int h, uint32_t tint,
                              int *ow, int *oh);

// 一次性解码并居中画进框里（不缓存）。用于每帧都在变、缓存没有意义的内容，
// 比如 ffmpeg 输出的视频预览帧。use_par 非零时按显示像素宽高比保持观感比例。
int skin_img_blit_file(uint32_t *dst, int dw, int dh, const char *path,
                       int bx, int by, int bw, int bh, int use_par);

// 把已解码的 ARGB 图按 alpha 混合到目标缓冲的 (dx, dy)。
void skin_img_blend(uint32_t *dst, int dw, int dh,
                    const uint32_t *src, int sw, int sh, int dx, int dy);
// 同上，但居中放进 (bx, by, bw, bh) 框内。返回实际绘制的矩形。
void skin_img_blend_box(uint32_t *dst, int dw, int dh,
                        const uint32_t *src, int sw, int sh,
                        int bx, int by, int bw, int bh,
                        int *out_x, int *out_y, int *out_w, int *out_h);

#endif // FBUI_SKIN_H
