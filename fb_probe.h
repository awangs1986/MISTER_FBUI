#ifndef FB_PROBE_H
#define FB_PROBE_H

#include <stdint.h>

// HPS FB smoke test (Scripts/F9 replace path).
// Disables OSD while active so mux visibility is unambiguous.
// Menu/Select exits the probe.

int fb_probe_red_begin();
void fb_probe_red_end();
void fb_probe_red_refresh();
int fb_probe_active();

#endif
