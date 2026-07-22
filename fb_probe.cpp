#include "fb_probe.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#include "video.h"
#include "input.h"
#include "osd.h"
#include "user_io.h"

static int fb_probe_on = 0;
static int fb_probe_phase = 0;

static void status_write(const char *line, int append)
{
	FILE *f = fopen("/tmp/fb_probe_status.txt", append ? "a" : "w");
	if (f)
	{
		fprintf(f, "%s\n", line);
		fclose(f);
	}
	printf("fb_probe: %s\n", line);
}

static int fill_dev_fb0(uint32_t rgb888)
{
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd < 0) return -1;

	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0)
	{
		close(fd);
		return -2;
	}

	size_t sz = finfo.smem_len;
	void *map = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
	{
		close(fd);
		return -3;
	}

	uint32_t swap = ((rgb888 & 0xFFu) << 16) | (rgb888 & 0xFF00u) | ((rgb888 >> 16) & 0xFFu);
	int stride = finfo.line_length / 4;
	uint32_t *p = (uint32_t *)map;
	int mid = (int)vinfo.xres / 2;
	for (int y = 0; y < (int)vinfo.yres; y++)
	{
		uint32_t *row = p + y * stride;
		for (int x = 0; x < (int)vinfo.xres; x++)
			row[x] = (x < mid) ? rgb888 : swap;
	}
	msync(map, sz, MS_SYNC);
	munmap(map, sz);
	close(fd);
	return (int)(vinfo.xres * vinfo.yres);
}

static void log_fpga_fb(const char *tag)
{
	uint16_t fmt = 0, w = 0, h = 0;
	int en = 0;
	video_fb_readback(&fmt, &w, &h, &en);
	char msg[192];
	snprintf(msg, sizeof(msg),
		"%s FPGA: LFB_EN=%d FB_EN=%d fmt=%04x %dx%d",
		tag, !!(fmt & 0x80), !!(fmt & 0x40), fmt, w, h);
	status_write(msg, 1);
}

static int apply_attempt(int phase, char *msg, size_t msglen)
{
	int use_rxb = 1;
	int plane = 0;
	uint32_t color = 0xFFFFFF;

	switch (phase % 5)
	{
	case 0: use_rxb = 1; plane = 0; color = 0xFFFFFF; break;
	case 1: use_rxb = 1; plane = 0; color = 0xFF0000; break;
	case 2: use_rxb = 0; plane = 0; color = 0xFF0000; break;
	case 3: use_rxb = 1; plane = 0; color = 0x0000FF; break;
	case 4: use_rxb = 1; plane = 1; color = 0xFFFF00; break;
	}

	video_fb_set_rxb(use_rxb);
	video_chvt(1);

	int ok = video_fb_fill_solid(color, plane);
	int nfb = fill_dev_fb0(color);
	video_fb_enable(1, plane);
	input_switch(1);

	snprintf(msg, msglen,
		"phase=%d ok=%d plane=%d rxb=%d color=%06x fb0=%d state=%d",
		phase % 5, ok, plane, use_rxb, color & 0xFFFFFF, nfb, video_fb_state());
	return ok;
}

int fb_probe_active()
{
	return fb_probe_on;
}

int fb_probe_red_begin()
{
	if (fb_probe_on) return 1;

	OsdDisable();
	OsdClear();

	char msg[192];
	fb_probe_phase = 0;
	status_write("BEGIN (vga_scaler should be 1 if using VGA)", 0);
	int ok = apply_attempt(fb_probe_phase, msg, sizeof(msg));
	status_write(msg, 1);
	log_fpga_fb("after enable");
	status_write("OSD off — expect FULLSCREEN color. Menu/Select exits.", 1);

	if (!ok)
	{
		status_write("FAIL: core rejected HPS FB or fb not ready", 1);
		video_fb_set_rxb(1);
		return 0;
	}

	fb_probe_on = 1;
	return 1;
}

void fb_probe_red_refresh()
{
	if (!fb_probe_on) return;

	static int ticks = 0;
	if (++ticks < 120)
	{
		video_fb_enable(1, (fb_probe_phase % 5 == 4) ? 1 : 0);
		input_switch(1);
		return;
	}
	ticks = 0;
	fb_probe_phase++;
	char msg[192];
	apply_attempt(fb_probe_phase, msg, sizeof(msg));
	status_write(msg, 1);
	log_fpga_fb("phase");
}

void fb_probe_red_end()
{
	if (!fb_probe_on) return;
	fb_probe_on = 0;
	fb_probe_phase = 0;
	video_fb_set_rxb(1);
	video_fb_enable(0);
	input_switch(1);
	OsdEnable(DISABLE_KEYBOARD);
	status_write("END", 1);
	printf("fb_probe: end, core video restored\n");
}
