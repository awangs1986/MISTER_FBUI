// FBUI 皮肤引擎实现 —— 见 fbui_skin.h 的格式说明。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

#include "fbui_skin.h"
#include "file_io.h"
#include "lib/imlib2/Imlib2.h"

// ---------------------------------------------------------------------------
// 内建默认值
// ---------------------------------------------------------------------------

// 这些默认值必须和重做之前的硬编码几何完全一致，这样没有皮肤包时观感不变。
#define DEF_BG_TOP    0x1A2029
#define DEF_BG_BOT    0x0D1015
#define DEF_HEADER    0x232B38
#define DEF_TEXT      0xC8D0DA
#define DEF_DIM       0x707A88
#define DEF_SEL_BG    0x2A6BD8
#define DEF_SEL_TEXT  0xFFFFFF
#define DEF_ACCENT    0x4A90E2
#define DEF_PANEL     0x161B24
#define DEF_TITLE     0xE0E0E0
#define DEF_MSG       0xFFB040

static skin_t sk;
static uint32_t *bg_cache = NULL;   // 已解码的 bg.png，尺寸恒为 sk.scr_w x sk.scr_h
static int bg_tried = 0;            // 0=未试过 1=已试过（无论成败），避免反复解码失败路径
static int thumb_explicit = 0;      // layout.ini 显式写了 thumb=，否则由面板框推导

static void skin_defaults(int scr_w, int scr_h)
{
	memset(&sk, 0, sizeof(sk));
	thumb_explicit = 0;
	sk.valid = 0;
	sk.scr_w = scr_w;
	sk.scr_h = scr_h;
	snprintf(sk.name, sizeof(sk.name), "builtin");

	// 分辨率档位。这是全工程唯一一处做这个判断的地方，其余代码一律读结构体。
	sk.lowres = (scr_h <= 288);
	sk.scale = (scr_h >= 900) ? 2 : 1;

	// 640x240 在 4:3 屏上像素宽度只有高度的一半
	sk.par_num = 1;
	sk.par_den = sk.lowres ? 2 : 1;

	sk.col_bg_top = DEF_BG_TOP;
	sk.col_bg_bot = DEF_BG_BOT;
	sk.col_header = DEF_HEADER;
	sk.col_panel = DEF_PANEL;
	sk.col_text = DEF_TEXT;
	sk.col_dim = DEF_DIM;
	sk.col_sel_bar = DEF_SEL_BG;
	sk.col_sel_text = DEF_SEL_TEXT;
	sk.col_accent = DEF_ACCENT;
	sk.col_title = DEF_TITLE;
	sk.col_msg = DEF_MSG;

	sk.list_align = SKIN_ALIGN_LEFT;

	if (sk.lowres)
	{
		// 240p 专用几何：22px 行高，可见七行列表加一个大媒体面板。
		sk.safe_l = sk.safe_r = scr_w * 6 / 100;
		sk.safe_t = sk.safe_b = scr_h * 6 / 100;
		sk.header_h = 24;
		sk.footer_h = 24;
		sk.row_h = 22;
		sk.row_pad = 4;
		sk.scrollbar_w = 3;
		sk.list_x = sk.safe_l + 8;
		sk.list_y = sk.safe_t + sk.header_h + 4;
		sk.list_w = (scr_w - sk.safe_l - sk.safe_r) * 58 / 100 - 8;
		sk.panel_x = sk.safe_l + (scr_w - sk.safe_l - sk.safe_r) * 60 / 100;
		sk.panel_y = sk.list_y;
		sk.panel_w = scr_w - sk.safe_r - sk.panel_x - 8;
		sk.panel_h = scr_h - sk.safe_b - sk.footer_h - 4 - sk.panel_y;
		sk.font_px = 16;
		sk.title_px = 16;
		sk.footer_px = 16;
		sk.cache_mb = 48;
	}
	else
	{
		int s = sk.scale;
		sk.safe_l = sk.safe_t = sk.safe_r = sk.safe_b = 0;
		sk.header_h = 28 * s;
		sk.footer_h = 24 * s;
		sk.row_h = 20 * s;
		sk.row_pad = 8 * s;
		sk.scrollbar_w = 3 * s;
		sk.list_x = 12 * s;
		sk.list_y = sk.header_h + 8 * s;
		sk.list_w = scr_w * 58 / 100 - 12 * s;
		sk.panel_x = scr_w * 60 / 100;
		sk.panel_y = sk.list_y;
		sk.panel_w = scr_w - sk.panel_x - 12 * s;
		sk.panel_h = scr_h - sk.panel_y - sk.footer_h - 8 * s;
		sk.font_px = 16 * s;
		sk.title_px = sk.font_px + sk.font_px / 3;
		sk.footer_px = sk.font_px * 4 / 5;
		sk.cache_mb = 128;
	}

	// showcase 的 logo 框
	int sw = scr_w - sk.safe_l - sk.safe_r;
	int sh = scr_h - sk.safe_t - sk.safe_b;
	sk.logo_x = sk.safe_l + sw * 10 / 100;
	sk.logo_y = sk.safe_t + sh * 18 / 100;
	sk.logo_w = sw * 80 / 100;
	sk.logo_h = sh * 40 / 100;

	// 固定尺寸资源槽就是面板框本身
	sk.thumb_w = sk.panel_w;
	sk.thumb_h = sk.panel_h;
	sk.sysbg_w = sk.lowres ? scr_w : 960;
	sk.sysbg_h = sk.lowres ? scr_h : 540;
	sk.syslogo_w = sk.logo_w;
	sk.syslogo_h = sk.logo_h;
}

static void clamp_geometry(void)
{
	if (sk.row_h < 1) sk.row_h = 1;
	if (sk.font_px < 8) sk.font_px = 8;
	if (sk.title_px < 8) sk.title_px = sk.font_px;
	if (sk.footer_px < 8) sk.footer_px = sk.font_px;
	if (sk.scrollbar_w < 1) sk.scrollbar_w = 1;
	if (sk.par_num < 1) sk.par_num = 1;
	if (sk.par_den < 1) sk.par_den = 1;

	// layout.ini 若改了面板但没写 thumb=，缩略图槽要跟着面板走，
	// 否则缓存预算会按一个和实际显示无关的尺寸去算。
	if (!thumb_explicit)
	{
		sk.thumb_w = sk.panel_w;
		sk.thumb_h = sk.panel_h;
	}
	if (sk.thumb_w < 8) sk.thumb_w = 8;
	if (sk.thumb_h < 8) sk.thumb_h = 8;
	if (sk.cache_mb < 8) sk.cache_mb = 8;
	if (sk.cache_mb > 192) sk.cache_mb = 192;

	// 可见行数由列表高度反推，layout.ini 不直接写行数，避免作者算错。
	int avail = sk.scr_h - sk.safe_b - sk.footer_h - 4 - sk.list_y;
	if (sk.list_h_override > 0) avail = sk.list_h_override;
	sk.list_rows = avail / sk.row_h;
	if (sk.list_rows < 1) sk.list_rows = 1;
}

// ---------------------------------------------------------------------------
// layout.ini 解析
// ---------------------------------------------------------------------------

// 从 "12,34,56,78" 这样的串里取最多 n 个整数，返回实际取到的个数。
static int parse_ints(const char *v, int *out, int n)
{
	int got = 0;
	const char *p = v;
	while (got < n && *p)
	{
		while (*p && !isdigit((unsigned char)*p) && *p != '-' && *p != '+') p++;
		if (!*p) break;
		out[got++] = (int)strtol(p, (char**)&p, 0);
	}
	return got;
}

static uint32_t parse_color(const char *v, uint32_t fallback)
{
	while (*v == ' ' || *v == '\t') v++;
	if (*v == '#') v++;
	else if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) v += 2;
	if (!isxdigit((unsigned char)*v)) return fallback;
	return (uint32_t)(strtoul(v, NULL, 16) & 0xFFFFFF);
}

static int parse_align(const char *v)
{
	if (!strncasecmp(v, "center", 6)) return SKIN_ALIGN_CENTER;
	if (!strncasecmp(v, "right", 5)) return SKIN_ALIGN_RIGHT;
	return SKIN_ALIGN_LEFT;
}

static void apply_kv(const char *key, const char *val)
{
	int n[4];

	if (!strcasecmp(key, "safe"))
	{
		if (parse_ints(val, n, 4) == 4)
		{
			sk.safe_l = n[0]; sk.safe_t = n[1]; sk.safe_r = n[2]; sk.safe_b = n[3];
		}
	}
	else if (!strcasecmp(key, "list"))
	{
		if (parse_ints(val, n, 4) == 4)
		{
			sk.list_x = n[0]; sk.list_y = n[1]; sk.list_w = n[2];
			sk.list_h_override = n[3];
		}
	}
	else if (!strcasecmp(key, "panel"))
	{
		if (parse_ints(val, n, 4) == 4)
		{
			sk.panel_x = n[0]; sk.panel_y = n[1]; sk.panel_w = n[2]; sk.panel_h = n[3];
		}
	}
	else if (!strcasecmp(key, "logo"))
	{
		if (parse_ints(val, n, 4) == 4)
		{
			sk.logo_x = n[0]; sk.logo_y = n[1]; sk.logo_w = n[2]; sk.logo_h = n[3];
			sk.syslogo_w = n[2]; sk.syslogo_h = n[3];
		}
	}
	else if (!strcasecmp(key, "thumb"))
	{
		if (parse_ints(val, n, 2) == 2)
		{
			sk.thumb_w = n[0];
			sk.thumb_h = n[1];
			thumb_explicit = 1;
		}
	}
	else if (!strcasecmp(key, "sysbg"))
	{
		if (parse_ints(val, n, 2) == 2) { sk.sysbg_w = n[0]; sk.sysbg_h = n[1]; }
	}
	else if (!strcasecmp(key, "par"))
	{
		if (parse_ints(val, n, 2) == 2) { sk.par_num = n[0]; sk.par_den = n[1]; }
	}
	else if (!strcasecmp(key, "header_h")) parse_ints(val, &sk.header_h, 1);
	else if (!strcasecmp(key, "footer_h")) parse_ints(val, &sk.footer_h, 1);
	else if (!strcasecmp(key, "row_h")) parse_ints(val, &sk.row_h, 1);
	else if (!strcasecmp(key, "row_pad")) parse_ints(val, &sk.row_pad, 1);
	else if (!strcasecmp(key, "scrollbar_w")) parse_ints(val, &sk.scrollbar_w, 1);
	else if (!strcasecmp(key, "font_px")) parse_ints(val, &sk.font_px, 1);
	else if (!strcasecmp(key, "title_px")) parse_ints(val, &sk.title_px, 1);
	else if (!strcasecmp(key, "footer_px")) parse_ints(val, &sk.footer_px, 1);
	else if (!strcasecmp(key, "cache_mb")) parse_ints(val, &sk.cache_mb, 1);
	else if (!strcasecmp(key, "showcase")) parse_ints(val, &sk.showcase, 1);
	else if (!strcasecmp(key, "list_align")) sk.list_align = parse_align(val);
	else if (!strcasecmp(key, "font"))
	{
		// 相对路径按 SD 卡根目录解析，绝对路径原样使用
		if (val[0] == '/') snprintf(sk.font, sizeof(sk.font), "%s", val);
		else snprintf(sk.font, sizeof(sk.font), "%s/%s", getRootDir(), val);
	}
	else if (!strcasecmp(key, "col_bg_top")) sk.col_bg_top = parse_color(val, sk.col_bg_top);
	else if (!strcasecmp(key, "col_bg_bot")) sk.col_bg_bot = parse_color(val, sk.col_bg_bot);
	else if (!strcasecmp(key, "col_header")) sk.col_header = parse_color(val, sk.col_header);
	else if (!strcasecmp(key, "col_panel")) sk.col_panel = parse_color(val, sk.col_panel);
	else if (!strcasecmp(key, "col_text")) sk.col_text = parse_color(val, sk.col_text);
	else if (!strcasecmp(key, "col_dim")) sk.col_dim = parse_color(val, sk.col_dim);
	else if (!strcasecmp(key, "col_sel_bar")) sk.col_sel_bar = parse_color(val, sk.col_sel_bar);
	else if (!strcasecmp(key, "col_sel_text")) sk.col_sel_text = parse_color(val, sk.col_sel_text);
	else if (!strcasecmp(key, "col_accent")) sk.col_accent = parse_color(val, sk.col_accent);
	else if (!strcasecmp(key, "col_title")) sk.col_title = parse_color(val, sk.col_title);
	else if (!strcasecmp(key, "col_msg")) sk.col_msg = parse_color(val, sk.col_msg);
	else if (!strcasecmp(key, "name")) snprintf(sk.name, sizeof(sk.name), "%s", val);
}

static int parse_ini(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;

	char line[512];
	while (fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == ';' || *p == '[' || *p == '\r' || *p == '\n' || !*p) continue;

		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = 0;

		// 去掉键尾和值首尾的空白
		char *ke = eq - 1;
		while (ke >= p && (*ke == ' ' || *ke == '\t')) *ke-- = 0;
		char *v = eq + 1;
		while (*v == ' ' || *v == '\t') v++;
		char *ve = v + strlen(v) - 1;
		while (ve >= v && (*ve == '\r' || *ve == '\n' || *ve == ' ' || *ve == '\t')) *ve-- = 0;

		if (*p) apply_kv(p, v);
	}
	fclose(f);
	return 1;
}

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------

void skin_unload(void)
{
	free(bg_cache);
	bg_cache = NULL;
	bg_tried = 0;
}

int skin_load(const char *name, int scr_w, int scr_h)
{
	skin_unload();
	skin_defaults(scr_w, scr_h);

	if (name && name[0])
	{
		char base[1024];
		snprintf(base, sizeof(base), "%s/skins/%s", getRootDir(), name);

		// 一次开机只有一种分辨率是活的，所以只解析被选中的那个子目录。
		// 选中的档位缺失时退到另一个可用目录，而不是直接失败。
		const char *want = sk.lowres ? "240p" : "1080p";
		const char *other = sk.lowres ? "1080p" : "240p";
		char cand[1200], ini[1400];

		snprintf(cand, sizeof(cand), "%s/%s", base, want);
		snprintf(ini, sizeof(ini), "%s/layout.ini", cand);
		if (access(ini, R_OK))
		{
			snprintf(cand, sizeof(cand), "%s/%s", base, other);
			snprintf(ini, sizeof(ini), "%s/layout.ini", cand);
			if (!access(ini, R_OK))
				printf("FBUI skin: '%s' 缺少 %s 目录，退回到 %s\n", name, want, other);
		}
		if (access(ini, R_OK))
		{
			// 单分辨率皮肤包也允许把 layout.ini 直接放在根目录
			snprintf(cand, sizeof(cand), "%s", base);
			snprintf(ini, sizeof(ini), "%s/layout.ini", cand);
		}

		if (!access(ini, R_OK))
		{
			char sini[1400];
			snprintf(sini, sizeof(sini), "%s/skin.ini", base);
			parse_ini(sini);          // 可选：显示名等全局项
			if (parse_ini(ini))
			{
				snprintf(sk.dir, sizeof(sk.dir), "%s", cand);
				sk.valid = 1;
			}
		}
		else
		{
			printf("FBUI skin: 未找到皮肤包 '%s'，使用内建默认布局\n", name);
		}
	}

	clamp_geometry();

	printf("FBUI skin: %s %dx%d (%s) 列表 %d 行, 缩略图 %dx%d, 缓存 %d MiB\n",
		sk.valid ? sk.name : "builtin", sk.scr_w, sk.scr_h,
		sk.lowres ? "240p" : "HD", sk.list_rows, sk.thumb_w, sk.thumb_h, sk.cache_mb);
	return sk.valid;
}

const skin_t *skin(void)
{
	if (!sk.scr_w) skin_defaults(640, 480);
	return &sk;
}

int skin_asset_path(const char *rel, char *out, size_t sz)
{
	if (!out || !sz) return 0;
	out[0] = 0;
	if (!sk.valid || !sk.dir[0] || !rel || !rel[0]) return 0;
	snprintf(out, sz, "%s/%s", sk.dir, rel);
	if (access(out, R_OK)) { out[0] = 0; return 0; }
	return 1;
}

// ---------------------------------------------------------------------------
// 图像解码
// ---------------------------------------------------------------------------
//
// 后端仍是 Imlib2。原计划换成 stb_image 是为了省掉那 5 个 .so，但 video.cpp
// 画菜单壁纸、scaler.cpp 存截图都还在用它，库怎么都得链进来，换了只剩下
// 「自己写一个比 Imlib2 差的缩放器」这一个后果。
//
// 唯一要处理的是 Imlib2 自带的解码缓存：它按文件名缓存整幅原图，而这里每张图
// 只解一次、结果由上层的美术池按字节记账管理，留着它等于凭空多出一份没人记账
// 的副本（1080p 的封面原图动辄好几 MB）。所以第一次解码前把它关掉。
static void imlib_cache_off(void)
{
	static int done = 0;
	if (done) return;
	done = 1;
	imlib_set_cache_size(0);
}

// 把当前 imlib 上下文里的图拷成一块 malloc 的 ARGB8888。
static uint32_t *grab_current(int w, int h, int has_alpha)
{
	DATA32 *src = imlib_image_get_data_for_reading_only();
	if (!src) return NULL;
	uint32_t *out = (uint32_t*)malloc((size_t)w * h * 4);
	if (!out) return NULL;
	size_t n = (size_t)w * h;
	if (has_alpha) memcpy(out, src, n * 4);
	else for (size_t i = 0; i < n; i++) out[i] = ((uint32_t)src[i]) | 0xFF000000u;
	return out;
}

// 解码并缩放。cw/ch <= 0 表示用整幅图；dw/dh <= 0 表示保持裁剪后的原始尺寸。
static uint32_t *decode_crop_scaled(const char *path, int cx, int cy, int cw, int ch,
                                    int dw, int dh, int *ow, int *oh)
{
	if (!path || !path[0]) return NULL;
	imlib_cache_off();
	Imlib_Image img = imlib_load_image(path);
	if (!img) return NULL;

	imlib_context_set_image(img);
	int sw = imlib_image_get_width();
	int sh = imlib_image_get_height();
	int has_alpha = imlib_image_has_alpha();
	if (sw <= 0 || sh <= 0) { imlib_free_image(); return NULL; }

	if (cw <= 0 || ch <= 0) { cx = 0; cy = 0; cw = sw; ch = sh; }
	if (dw <= 0) dw = cw;
	if (dh <= 0) dh = ch;

	uint32_t *out = NULL;
	if (dw == sw && dh == sh && cw == sw && ch == sh)
	{
		// 尺寸已经对上，省掉一次缩放和一次分配
		out = grab_current(sw, sh, has_alpha);
		imlib_free_image();
	}
	else
	{
		Imlib_Image scaled = imlib_create_cropped_scaled_image(cx, cy, cw, ch, dw, dh);
		imlib_context_set_image(img);
		imlib_free_image();
		if (!scaled) return NULL;
		imlib_context_set_image(scaled);
		imlib_image_set_has_alpha(has_alpha);   // 缩放会丢掉这个标志
		out = grab_current(dw, dh, has_alpha);
		imlib_free_image();
	}

	if (!out) return NULL;
	if (ow) *ow = dw;
	if (oh) *oh = dh;
	return out;
}

static uint32_t *decode_scaled(const char *path, int dw, int dh, int *ow, int *oh)
{
	return decode_crop_scaled(path, 0, 0, 0, 0, dw, dh, ow, oh);
}

// 按 0xAARRGGBB 的乘法着色，0xFFFFFFFF 表示不变。
static void apply_tint(uint32_t *px, size_t n, uint32_t tint)
{
	if (!px || tint == 0xFFFFFFFFu) return;
	uint32_t ta = tint >> 24, tr = (tint >> 16) & 0xFF;
	uint32_t tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
	for (size_t i = 0; i < n; i++)
	{
		uint32_t c = px[i];
		uint32_t a = (c >> 24) * ta / 255;
		uint32_t r = ((c >> 16) & 0xFF) * tr / 255;
		uint32_t g = ((c >> 8) & 0xFF) * tg / 255;
		uint32_t b = (c & 0xFF) * tb / 255;
		px[i] = (a << 24) | (r << 16) | (g << 8) | b;
	}
}

uint32_t *skin_img_load(const char *path, int *w, int *h)
{
	return decode_scaled(path, 0, 0, w, h);
}

uint32_t *skin_img_load_exact(const char *path, int w, int h)
{
	if (w < 1 || h < 1) return NULL;
	return decode_scaled(path, w, h, NULL, NULL);
}

// 只探测尺寸，用于决定缩放目标后再一次性解码到位。
// imlib_load_image 只解文件头、像素按需才解，所以这里不会真的解一遍图。
static int probe_size(const char *path, int *w, int *h)
{
	imlib_cache_off();
	Imlib_Image img = imlib_load_image(path);
	if (!img) return 0;
	imlib_context_set_image(img);
	*w = imlib_image_get_width();
	*h = imlib_image_get_height();
	imlib_free_image();
	return (*w > 0 && *h > 0);
}

uint32_t *skin_img_load_fit(const char *path, int max_w, int max_h, int *w, int *h)
{
	int sw, sh;
	if (max_w < 1 || max_h < 1) return NULL;
	if (!probe_size(path, &sw, &sh)) return NULL;

	int dw = max_w, dh = (int)((int64_t)sh * max_w / sw);
	if (dh > max_h) { dh = max_h; dw = (int)((int64_t)sw * max_h / sh); }
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	return decode_scaled(path, dw, dh, w, h);
}

uint32_t *skin_img_load_fit_par(const char *path, int max_w, int max_h, int *w, int *h)
{
	int sw, sh;
	if (max_w < 1 || max_h < 1) return NULL;
	if (!probe_size(path, &sw, &sh)) return NULL;

	// 目标是"视觉上不变形"：一个像素显示出来是 par_num : par_den。
	// 想让宽高比为 sw:sh 的图看着正确，帧缓冲里的宽高必须满足
	// (dw * par_num) / (dh * par_den) == sw / sh。
	int64_t num = (int64_t)sw * sk.par_den;
	int64_t den = (int64_t)sh * sk.par_num;
	if (num < 1 || den < 1) return NULL;

	int dw = max_w;
	int dh = (int)((int64_t)dw * den / num);
	if (dh > max_h)
	{
		dh = max_h;
		dw = (int)((int64_t)dh * num / den);
	}
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	if (dw > max_w) dw = max_w;
	return decode_scaled(path, dw, dh, w, h);
}

uint32_t *skin_img_load_cover(const char *path, int w, int h, uint32_t tint,
                              int *ow, int *oh)
{
	if (ow) *ow = 0;
	if (oh) *oh = 0;
	if (w < 1 || h < 1) return NULL;

	int sw, sh;
	if (!probe_size(path, &sw, &sh)) return NULL;

	// 中心裁剪到目标框的比例再缩满，铺满不留黑边
	int cx = 0, cy = 0, cw = sw, ch = sh;
	if ((int64_t)sw * h > (int64_t)sh * w)
	{
		cw = (int)((int64_t)sh * w / h);
		cx = (sw - cw) / 2;
	}
	else
	{
		ch = (int)((int64_t)sw * h / w);
		cy = (sh - ch) / 2;
	}
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;

	uint32_t *px = decode_crop_scaled(path, cx, cy, cw, ch, w, h, ow, oh);
	apply_tint(px, (size_t)w * h, tint);
	return px;
}

// ---------------------------------------------------------------------------
// 混合
// ---------------------------------------------------------------------------

void skin_img_blend(uint32_t *dst, int dw, int dh,
                    const uint32_t *src, int sw, int sh, int dx, int dy)
{
	if (!dst || !src || sw < 1 || sh < 1) return;

	// 裁剪一次算清楚，内循环里不再做边界判断。
	int x0 = dx < 0 ? -dx : 0;
	int y0 = dy < 0 ? -dy : 0;
	int x1 = sw, y1 = sh;
	if (dx + x1 > dw) x1 = dw - dx;
	if (dy + y1 > dh) y1 = dh - dy;
	if (x0 >= x1 || y0 >= y1) return;

	for (int y = y0; y < y1; y++)
	{
		const uint32_t *s = src + (size_t)y * sw + x0;
		uint32_t *d = dst + (size_t)(dy + y) * dw + dx + x0;
		for (int x = x0; x < x1; x++, s++, d++)
		{
			uint32_t c = *s;
			uint32_t a = c >> 24;
			if (!a) continue;
			if (a >= 250) { *d = c & 0xFFFFFF; continue; }
			uint32_t o = *d;
			uint32_t ia = 255 - a;
			uint32_t r = (((c >> 16) & 255) * a + ((o >> 16) & 255) * ia) / 255;
			uint32_t g = (((c >> 8) & 255) * a + ((o >> 8) & 255) * ia) / 255;
			uint32_t b = ((c & 255) * a + (o & 255) * ia) / 255;
			*d = (r << 16) | (g << 8) | b;
		}
	}
}

void skin_img_blend_box(uint32_t *dst, int dw, int dh,
                        const uint32_t *src, int sw, int sh,
                        int bx, int by, int bw, int bh,
                        int *out_x, int *out_y, int *out_w, int *out_h)
{
	if (out_x) *out_x = bx;
	if (out_y) *out_y = by;
	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!src || sw < 1 || sh < 1 || bw < 1 || bh < 1) return;

	// 缓存里的图已经是按框预缩放过的，这里只负责居中，不再缩放。
	int px = bx + (bw - sw) / 2;
	int py = by + (bh - sh) / 2;
	skin_img_blend(dst, dw, dh, src, sw, sh, px, py);

	if (out_x) *out_x = px;
	if (out_y) *out_y = py;
	if (out_w) *out_w = sw;
	if (out_h) *out_h = sh;
}

int skin_img_blit_file(uint32_t *dst, int dw, int dh, const char *path,
                       int bx, int by, int bw, int bh, int use_par)
{
	int w = 0, h = 0;
	uint32_t *px = use_par ? skin_img_load_fit_par(path, bw, bh, &w, &h)
	                       : skin_img_load_fit(path, bw, bh, &w, &h);
	if (!px) return 0;

	skin_img_blend_box(dst, dw, dh, px, w, h, bx, by, bw, bh, NULL, NULL, NULL, NULL);
	free(px);
	return 1;
}

// ---------------------------------------------------------------------------
// 预合成静态层
// ---------------------------------------------------------------------------

int skin_fill_background(uint32_t *dst, int w, int h)
{
	if (!dst || w != sk.scr_w || h != sk.scr_h) return 0;

	if (!bg_tried)
	{
		bg_tried = 1;
		char path[1300];
		if (skin_asset_path("bg.png", path, sizeof(path)) ||
		    skin_asset_path("bg.jpg", path, sizeof(path)))
		{
			// 皮肤作者出的图就是整屏尺寸；万一不是，这里一次性拉到位并缓存，
			// 之后每帧都只剩一次 memcpy。
			bg_cache = skin_img_load_exact(path, sk.scr_w, sk.scr_h);
			if (bg_cache) printf("FBUI skin: 背景层 %s\n", path);
			else printf("FBUI skin: 背景层解码失败 %s\n", path);
		}
	}

	if (!bg_cache) return 0;
	memcpy(dst, bg_cache, (size_t)w * h * 4);
	return 1;
}
