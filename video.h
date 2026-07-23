#ifndef VIDEO_H
#define VIDEO_H

#define VFILTER_HORZ  0
#define VFILTER_VERT  1
#define VFILTER_SCAN  2
#define VFILTER_ILACE 3

struct VideoInfo
{
	uint32_t width;
	uint32_t height;
	uint32_t htime;
	uint32_t vtime;
	uint32_t ptime;
	uint32_t ctime;
	uint32_t vtimeh;
	uint32_t arx;
	uint32_t ary;
	uint32_t arxy;
	uint32_t fb_en;
	uint32_t fb_fmt;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t pixrep;
	uint32_t de_h;
	uint32_t de_v;

	bool interlaced;
	bool rotated;
};

// expose video timings for timerfd-based frame timer
extern VideoInfo current_video_info;

void  video_init();
void  video_poll();

int   video_get_edid(uint8_t **buf, int *size);
void  video_hdmi_power(int on);

int   video_get_scaler_flt(int type);
void  video_set_scaler_flt(int type, int n);
char* video_get_scaler_coeff(int type, int only_name = 1);
void  video_set_scaler_coeff(int type, const char *name);

int   video_get_gamma_en();
void  video_set_gamma_en(int n);
char* video_get_gamma_curve(int only_name = 1);
void  video_set_gamma_curve(const char *name);

int   video_get_shadow_mask_mode();
void  video_set_shadow_mask_mode(int n);
char* video_get_shadow_mask(int only_name = 1);
void  video_set_shadow_mask(const char *name);
void  video_loadPreset(char *name, bool save);

int   video_get_rotated();

void video_cfg_reset();

void  video_mode_adjust(bool force = false);

int   hasAPI1_5();

void video_fb_enable(int enable, int n = 0);
int video_fb_state();
// 1 (default): FB_FMT_RxB; 0: no R/B swap. Used by fb_probe.
void video_fb_set_rxb(int enable);
// Read back FPGA FB params after UIO_SET_FBUF. Returns 1 if SPI ok.
int video_fb_readback(uint16_t *fb_fmt, uint16_t *fb_w, uint16_t *fb_h, int *fb_en);
// Enable HPS FB plane and fill solid XRGB color. Returns 1 on success.
int video_fb_fill_solid(uint32_t rgb888, int plane = 0);
// Raw pixel access to wallpaper planes (1 or 2) for FBUI. Returns NULL if FB
// is not mapped yet. w/h are the current framebuffer dimensions.
uint32_t* video_fb_get_plane(int n, int *w, int *h);
void video_menu_bg(int n, int idle = 0);
int video_bg_has_picture();
int video_chvt(int num);
void video_cmd(char *cmd);
void video_mode_cmd(char *cmd);

// FBUI boot resolution: temporarily unify HDMI+VGA to one mode.
// is_1080=0 -> NTSC 15kHz 240p; is_1080=1 -> restore pre-picker HD mode.
// First call saves INI vga_scaler + current timings; restore brings them back.
void video_menu_res_apply(int is_1080);
void video_menu_res_restore();
int video_menu_res_active();

void video_core_description(char *str, size_t len);
void video_scaler_description(char *str, size_t len);
char* video_get_core_mode_name(int with_vrefresh = 1);

void dbg_draw_cursor(int x, int y);

#endif // VIDEO_H
