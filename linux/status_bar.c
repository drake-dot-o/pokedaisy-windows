/* The in-game status bar (status_bar.h): GameStatusBar.kt in C, drawn at the game's own width so the host can scale it
 * with the picture. */
#include "status_bar.h"

#include "pd_style.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define BAR_TEXT 0x20242C
#define BAR_SHADOW 0xFFFFFF
#define BAR_ACCENT 0xC8321E

static const char* const POKE_DOLLAR[] = {
    ".oooo.", ".o...o", ".o...o", ".oooo.", ".o....", "oooo..", ".o....", "oooo..", ".o....", NULL,
};
static const char* const MAP_PIN[] = {
    ".ooooo.", "ooooooo", "ooo.ooo", "oo...oo", "ooo.ooo", ".ooooo.", "..ooo..", "..ooo..", "...o...", NULL,
};

static void art(struct pd_canvas* c, const char* const* rows, int x, int y, uint32_t color, uint32_t shadow) {
    if (shadow != PD_NO_SHADOW)
        for (int row = 0; rows[row]; row++)
            for (int col = 0; rows[row][col]; col++)
                if (rows[row][col] == 'o')
                    pd_fill(c, x + col + 1, y + row + 1, 1, 1, shadow);
    for (int row = 0; rows[row]; row++)
        for (int col = 0; rows[row][col]; col++)
            if (rows[row][col] == 'o')
                pd_fill(c, x + col, y + row, 1, 1, color);
}

/* The app's gameCaps: capitals, keeping UTF-8's accented letters as they are. */
static void caps(char* out, size_t size, const char* in) {
    size_t n = 0;
    if (!in)
        in = "";
    for (const unsigned char* p = (const unsigned char*)in; *p && n + 1 < size; p++) {
        if (*p < 0x80 && islower(*p))
            out[n++] = (char)toupper(*p);
        else
            out[n++] = (char)*p;
    }
    out[n] = 0;
}

void status_bar_draw(struct pd_canvas* c, const struct status_bar_info* s) {
    const int w = c->w, h = c->h;
    if (w < 40 || h < 12)
        return;
    // The OPTION screen's white title window: a grey rim, then the paper.
    pd_round_rect(c, 0, 0, w, h, 2, 0xC8C8CE);
    pd_round_rect(c, 1, 1, w - 2, h - 2, 2, 0xFFFFFF);
    const int y = pd_text_y(0, h);
    const int inner = 6;
    int right = w - inner;

    if (s->battery >= 0) {
        int bw = pd_battery_width(s->battery, s->charging != 0);
        right -= bw;
        pd_battery_draw(c, right + bw, y, s->battery, s->charging != 0);
        right -= 10;
    }
    if (s->clock && *s->clock) {
        int tw = pd_text_width(s->clock);
        right -= tw;
        pd_text(c, right, y, s->clock, BAR_TEXT, BAR_SHADOW);
        right -= 10;
    }
    if (s->money >= 0) {
        char money[24];
        snprintf(money, sizeof money, "%d", s->money);
        int mw = pd_text_width(money), total = 7 + mw; // the sign and its shadow, then a pixel
        right -= total;
        art(c, POKE_DOLLAR, right, y, BAR_TEXT, BAR_SHADOW);
        pd_text(c, right + 7, y, money, BAR_TEXT, BAR_SHADOW);
        right -= 10;
    }

    char title[96], place[64];
    caps(title, sizeof title, s->title);
    caps(place, sizeof place, s->place);
    const int left = inner, limit = right - 2;
    if (limit <= left)
        return;
    int room = limit - left;
    if (!place[0]) {
        pd_text_fit(c, left, y, room, title, BAR_TEXT, BAR_SHADOW);
        return;
    }
    const int pinW = 8, gap = 10;
    int titleW = pd_text_width(title);
    int locW = pd_text_width(place);
    if (titleW + gap + pinW + 2 + locW <= room) {
        pd_text(c, left, y, title, BAR_TEXT, BAR_SHADOW);
        int x = left + titleW + gap;
        art(c, MAP_PIN, x, y, BAR_ACCENT, BAR_SHADOW);
        pd_text(c, x + pinW + 2, y, place, BAR_TEXT, BAR_SHADOW);
        return;
    }
    // A long name gives way before the place, as the app's weighted Row does.
    int titleMax = room * 55 / 100;
    int titleDrawn = pd_text_fit(c, left, y, titleMax, title, BAR_TEXT, BAR_SHADOW);
    int x = left + titleDrawn + gap;
    if (x + pinW + 2 >= limit)
        return;
    art(c, MAP_PIN, x, y, BAR_ACCENT, BAR_SHADOW);
    pd_text_fit(c, x + pinW + 2, y, limit - (x + pinW + 2), place, BAR_TEXT, BAR_SHADOW);
}