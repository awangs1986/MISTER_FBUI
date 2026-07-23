// FBUI — full-screen graphical game browser drawn by ARM into the HPS
// framebuffer. Menu core only (uses the wallpaper planes 1/2 of the scaler
// framebuffer path, so the hardware OSD and input routing stay intact).
//
// Flow: menu.cpp calls fbui_ui_hook() right after menu_key_get(). While the
// UI is active all keys are consumed here and HandleUI returns early.
// Selecting a game writes /tmp/fbui.mgl and boots it via xml_load().

#ifndef FBUI_H
#define FBUI_H

#include <inttypes.h>

// Returns: 0 = not active, key not consumed (normal OSD menu continues);
//          1 = consumed (FBUI active or just started);
//          2 = FBUI just exited (caller should reset OSD menu state).
int fbui_ui_hook(uint32_t *pc);

int fbui_active();

// unifont record for fbui_theme fallback rendering:
// [0] = width flag (0 = 8px, 1 = 16px), [1..32] = row bitmaps (MSB left).
// NULL when the glyph is missing or the .hex font was not loaded.
const uint8_t *fbui_hexfont_get(uint32_t cp);

#endif // FBUI_H
