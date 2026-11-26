/* The in-game status bar: the app's GameStatusBar over the top of the game. */
#ifndef POCKET_STATUS_BAR_H
#define POCKET_STATUS_BAR_H

#include "pd_canvas.h"

struct status_bar_info {
    const char* title;  // the game's name, UTF-8 (NULL / "" = none)
    const char* place;  // the map section, UTF-8 (NULL / "" = none)
    int money;          // -1 = unknown
    const char* clock;  // "14:05"
    int battery;        // 0-100, -1 = no battery reading
    int charging;
};

/* Draws into c (which the caller sized): the white title window strip, the title and place on the left, money, clock
 * and battery on the right. */
void status_bar_draw(struct pd_canvas* c, const struct status_bar_info* s);

#endif