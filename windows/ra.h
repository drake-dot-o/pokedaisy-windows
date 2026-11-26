// RetroAchievements for the Windows / UWP host (rcheevos' rc_client).
//
// The host owns the emulator and the memory map: it passes pd_ra_init a read
// callback that maps a RetroAchievements address into the core's memory, and
// pd_ra_game_start the console id and ROM path whose hash names the game.
// This file is only built where PD_RETROACHIEVEMENTS is defined.
#ifndef PD_RA_H
#define PD_RA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PD_RA_MAX_ROWS 96

// One row of the host's RETROACHIEVEMENTS page.
enum {
    PD_RA_ROW_INFO = 0,
    PD_RA_ROW_SIGN_IN,
    PD_RA_ROW_SIGN_OUT,
    PD_RA_ROW_NOTIFICATIONS,
    PD_RA_ROW_ACHIEVEMENT,
};

// The text prompt's kind.
enum {
    PD_RA_PROMPT_NONE = 0,
    PD_RA_PROMPT_USERNAME,
    PD_RA_PROMPT_PASSWORD,
};

typedef uint32_t (*pd_ra_read_memory_fn)(uint32_t address, uint8_t *buffer, uint32_t num_bytes, void *ctx);

// Starts rc_client and signs in again with the saved token, if any. dataDir is
// where ra.ini lives (the host's LocalState); version goes in the User-Agent.
void pd_ra_init(const char *dataDir, const char *version, pd_ra_read_memory_fn read, void *readCtx);
void pd_ra_shutdown(void);

// The game whose hash names the set: console is an RC_CONSOLE_* id. Call after
// the core loaded and published its memory map.
void pd_ra_game_start(int console, const char *romPath);
void pd_ra_game_stop(void);

// Per emulated frame. paused = the game is not advancing (a rewind), so checks
// are held back. Responses are always delivered.
void pd_ra_frame(bool paused);
// Delivers server answers while no frame runs (login, while a menu is open).
void pd_ra_pump(void);

// The next notification to show, or NULL.
const char *pd_ra_popup(void);

// The RETROACHIEVEMENTS page. Rows are static until the next pd_ra_menu_build.
int pd_ra_menu_count(void);
const char *pd_ra_menu_row(int row);
int pd_ra_menu_action(int row);
void pd_ra_menu_detail(int row, char *out, size_t size);
void pd_ra_menu_open(void);

// Prompt for the account. Toggle the notification popups.
void pd_ra_sign_in(void);
void pd_ra_sign_out(void);
void pd_ra_toggle_notifications(void);

// The prompt, while one is open. text is UTF-8 from SDL_TEXTINPUT.
int pd_ra_prompt(void);
const char *pd_ra_prompt_title(void);
const char *pd_ra_prompt_value(void);
void pd_ra_prompt_text(const char *utf8);
void pd_ra_prompt_backspace(void);
// True when the prompt is finished (the password was submitted, or cancelled).
bool pd_ra_prompt_submit(void);
void pd_ra_prompt_cancel(void);

// A save state's achievement progress, beside it as <state>.ra.
void pd_ra_save_progress(const char *statePath);
void pd_ra_load_progress(const char *statePath);

#endif
