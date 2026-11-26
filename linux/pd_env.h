/* Private libretro environment commands shared by PokeDaisy's cores and host. */
#ifndef PD_ENV_H
#define PD_ENV_H

#include <stdint.h>

/* The core's quick menu asks the frontend to turn fast-forward on or off. [data] is a bool. */
#define PD_ENV_SET_FAST_FORWARD 0x20001

/* The core's floating touch screen. The core keeps the game's bottom screen at its full 256 x 192 and asks a host that
 * can draw it to do the output scaling itself, instead of scaling the pixels down into the panel first. [data] is a
 * pd_touch_panel; the pointer stays valid until the core's next frame. active = 0 withdraws the panel. */
#define PD_ENV_SET_TOUCH_PANEL 0x20002
struct pd_touch_panel {
  const uint32_t *pixels; /* src_w x src_h, XRGB8888 (0xFFRRGGBB), as the frame's pixels are */
  int src_w, src_h;
  int x, y, w, h;         /* the screen's place in the core's canvas */
  int active;
};

/* The core's field HUD at its own resolution. A ZOOM step grows the core's canvas and the frontend scales it down to
 * the window, so a HUD blended into the picture is resampled with it: sharp at whole steps, soft at 1.25x and 1.5x. A
 * host that can draw it takes the HUD's own picture instead, drawn over its box in the window's pixels, so each HUD
 * pixel reaches the window from the picture once. [data] is a pd_hud; the pixels stay valid until the core's next
 * frame. active = 0 withdraws it. */
#define PD_ENV_SET_HUD 0x20003
struct pd_hud {
  const uint32_t *pixels; /* src_w x src_h, the HUD's own 256x192, ARGB8888, alpha 0 = clear */
  int src_w, src_h;
  int x, y, w, h;         /* the HUD's box in the core's canvas (the 1x top screen's place at every ZOOM step) */
  int pct;                /* 100: the picture over the whole box. Less: each quarter at its own corner, scaled by pct */
  int active;
};

/* The running game's status-bar facts (the app's GameStatusBar): the game's name, the place, and money where the game
 * has it. A host that draws the bar itself (the SDL hosts) takes them with each frame; another frontend answers false
 * and the core keeps them to itself. [title] / [place] are UTF-8 and stay valid until the core's next frame; empty / NULL
 * means unknown. enabled = the companion's STATUS BAR setting; active = 0 hides the bar just now (a companion menu, a
 * battle transition, no game). */
#define PD_ENV_SET_STATUS_BAR 0x20004
struct pd_status_bar {
  const char *title, *place;
  int money;     /* -1 = unknown */
  int enabled, active;
};

/* The SDL hosts' status bar: its whole-pixel scale for a window [h] pixels tall, and the strip's height. A core that
 * lays its canvas out for the area under the bar (the DS core) uses the same rule, so both agree on what is left. */
enum { PD_STATUS_BAR_HEIGHT = 20 };
static inline int pd_status_bar_scale(int h) {
  const int s = h / 240;
  return s < 1 ? 1 : s > 4 ? 4 : s;
}
static inline int pd_status_bar_height(int h) { return PD_STATUS_BAR_HEIGHT * pd_status_bar_scale(h); }

#endif
