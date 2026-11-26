// RetroAchievements for the Windows / UWP host (rcheevos' rc_client).
//
// rc_client is driven on the host's main thread; server calls are handed to one
// worker thread that uses WinHTTP and are delivered back by ra_pump, because
// rc_client reads game memory while finishing a game load. The account is kept
// in ra.ini beside the host's other data (username + the token the server
// returns; the password is only passed to rc_client for the login call).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "ra.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#if defined(PD_UWP)
// The UWP build's SDK winhttp.h spells one parameter "LPCWSTR FAR *"; MSVC's C front end leaves FAR as the
// legacy far keyword there, which it no longer parses. The pointer means the same without it.
#undef FAR
#define FAR
#endif
#include <winhttp.h>

#include <SDL.h>

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_error.h"
#include "rc_hash.h"

#define RA_MAX_CALLS 32
#define RA_MAX_POPUPS 12
#define RA_POPUP_LEN 160
#define RA_ROW_LEN 96
#define RA_USER_LEN 64
#define RA_TOKEN_LEN 256
#define RA_NAME_LEN 96

static rc_client_t *ra_client;
static pd_ra_read_memory_fn ra_read;
static void *ra_read_ctx;
static char ra_data_dir[1024];
static char ra_user_agent[128];
static char ra_ini[1200];

static bool ra_ready;
static bool ra_signing_in;
static bool ra_game_active;
static char ra_hash[33];
static char ra_user[RA_USER_LEN];
static char ra_display[RA_NAME_LEN];
static char ra_token[RA_TOKEN_LEN];
static bool ra_notify = true;
static char ra_last_error[RA_POPUP_LEN];

static bool ra_game_loading;
static bool ra_game_waiting_login;
static char ra_game_title[RA_NAME_LEN];
static uint32_t ra_game_id;
static uint32_t ra_total, ra_unlocked, ra_points, ra_points_unlocked;
static bool ra_offline_shown;

// --- server calls -----------------------------------------------------------

typedef struct {
    int id;
    rc_client_server_callback_t callback;
    void *callback_data;
    int busy;
    int ready;
    int status;
    char *url;
    char *post;
    char *content_type;
    char *body;
    size_t body_len;
} ra_call;

static ra_call ra_calls[RA_MAX_CALLS];
static int ra_next_id = 1;
static SDL_mutex *ra_calls_lock;
static SDL_mutex *ra_pump_lock;
static SDL_cond *ra_calls_cond;
static SDL_Thread *ra_http_thread;
static int ra_http_quit;

// --- popups -----------------------------------------------------------------

static char ra_popups[RA_MAX_POPUPS][RA_POPUP_LEN];
static int ra_popup_head, ra_popup_count;

static void ra_push_popup(const char *fmt, ...) {
    if (!ra_notify || ra_popup_count >= RA_MAX_POPUPS)
        return;
    char *out = ra_popups[(ra_popup_head + ra_popup_count) % RA_MAX_POPUPS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, RA_POPUP_LEN, fmt, ap);
    va_end(ap);
    ra_popup_count++;
}

const char *pd_ra_popup(void) {
    if (!ra_popup_count)
        return NULL;
    static char out[RA_POPUP_LEN];
    snprintf(out, sizeof out, "%s", ra_popups[ra_popup_head]);
    ra_popup_head = (ra_popup_head + 1) % RA_MAX_POPUPS;
    ra_popup_count--;
    return out;
}

// --- settings (ra.ini) ------------------------------------------------------

static void ra_settings_save(void) {
    if (!ra_ini[0])
        return;
    FILE *f = fopen(ra_ini, "w");
    if (!f)
        return;
    fprintf(f, "user=%s\ntoken=%s\nnotify=%d\n", ra_user, ra_token, ra_notify ? 1 : 0);
    fclose(f);
}

static void ra_settings_load(void) {
    ra_ini[0] = 0;
    if (ra_data_dir[0])
        snprintf(ra_ini, sizeof ra_ini, "%s/ra.ini", ra_data_dir);
    FILE *f = ra_ini[0] ? fopen(ra_ini, "r") : NULL;
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "user=", 5))
            snprintf(ra_user, sizeof ra_user, "%s", line + 5);
        else if (!strncmp(line, "token=", 6))
            snprintf(ra_token, sizeof ra_token, "%s", line + 6);
        else if (!strncmp(line, "notify=", 7))
            ra_notify = atoi(line + 7) != 0;
    }
    fclose(f);
}

// --- HTTP -------------------------------------------------------------------

typedef struct {
    char *body;
    size_t len;
    int status;
} ra_http_result;

// The calls' URL / body copies live in this module's heap: SDL_strdup allocates in SDL2.dll's CRT, and freeing that
// with the host's free (or the other way round) corrupts the heap on Windows. malloc and free here are the same CRT.
static char *ra_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t len = strlen(text) + 1;
    char *copy = (char *)malloc(len);
    if (copy)
        memcpy(copy, text, len);
    return copy;
}

static bool ra_http_do(const char *url, const char *post, const char *content_type, ra_http_result *out) {
    memset(out, 0, sizeof *out);
    if (!url)
        return false;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    if (wlen <= 0)
        return false;
    wchar_t *wurl = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wurl)
        return false;
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, wlen);

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        free(wurl);
        return false;
    }

    wchar_t host[256];
    size_t host_len = uc.dwHostNameLength < 255 ? uc.dwHostNameLength : 255;
    memcpy(host, uc.lpszHostName, host_len * sizeof(wchar_t));
    host[host_len] = 0;

    wchar_t path[2048];
    size_t path_len = uc.dwUrlPathLength < 1000 ? uc.dwUrlPathLength : 1000;
    memcpy(path, uc.lpszUrlPath, path_len * sizeof(wchar_t));
    if (uc.dwExtraInfoLength && path_len + uc.dwExtraInfoLength < 2000) {
        memcpy(path + path_len, uc.lpszExtraInfo, uc.dwExtraInfoLength * sizeof(wchar_t));
        path_len += uc.dwExtraInfoLength;
    }
    path[path_len] = 0;
    if (!path_len) {
        path[0] = L'/';
        path[1] = 0;
    }

    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;

    wchar_t ua[256];
    MultiByteToWideChar(CP_UTF8, 0, ra_user_agent[0] ? ra_user_agent : "PokeDaisy", -1, ua,
                        (int)(sizeof ua / sizeof ua[0]));

    HINTERNET session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        free(wurl);
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        free(wurl);
        return false;
    }

    const wchar_t *method = post ? L"POST" : L"GET";
    HINTERNET request = WinHttpOpenRequest(connect, method, path, NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        free(wurl);
        return false;
    }

    DWORD post_len = post ? (DWORD)strlen(post) : 0;
    wchar_t headers[512] = L"";
    if (content_type && content_type[0]) {
        wchar_t type[256];
        MultiByteToWideChar(CP_UTF8, 0, content_type, -1, type, (int)(sizeof type / sizeof type[0]));
        swprintf(headers, sizeof headers / sizeof headers[0], L"Content-Type: %ls\r\n", type);
    }
    if (post_len)
        swprintf(headers + wcslen(headers), sizeof headers / sizeof headers[0] - wcslen(headers),
                 L"Content-Length: %lu\r\n", (unsigned long)post_len);
    BOOL sent = WinHttpSendRequest(request, headers[0] ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                                   headers[0] ? (DWORD)-1L : 0, post ? (LPVOID)post : WINHTTP_NO_REQUEST_DATA,
                                   post_len, post_len, 0);
    bool ok = sent && WinHttpReceiveResponse(request, NULL);
    if (ok) {
        DWORD status = 0, status_len = sizeof status;
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX))
            status = 0;
        out->status = (int)status;

        size_t cap = 4096, len = 0;
        char *body = (char *)malloc(cap + 1);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail) || !avail)
                break;
            if (len + avail > cap) {
                while (len + avail > cap)
                    cap *= 2;
                char *grown = (char *)realloc(body, cap + 1);
                if (!grown) {
                    ok = false;
                    break;
                }
                body = grown;
            }
            DWORD got = 0;
            if (!WinHttpReadData(request, body + len, avail, &got) || !got)
                break;
            len += got;
        }
        if (ok) {
            body[len] = 0;
            out->body = body;
            out->len = len;
        } else {
            free(body);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    free(wurl);
    return ok;
}

static int SDLCALL ra_http_thread_fn(void *unused) {
    (void)unused;
    for (;;) {
        ra_call *call = NULL;
        SDL_LockMutex(ra_calls_lock);
        for (;;) {
            if (ra_http_quit)
                break;
            for (int i = 0; i < RA_MAX_CALLS; i++) {
                if (ra_calls[i].id && !ra_calls[i].busy && !ra_calls[i].ready) {
                    call = &ra_calls[i];
                    break;
                }
            }
            if (call)
                break;
            SDL_CondWaitTimeout(ra_calls_cond, ra_calls_lock, 100);
        }
        if (ra_http_quit) {
            SDL_UnlockMutex(ra_calls_lock);
            break;
        }
        call->busy = 1;
        int id = call->id;
        char *url = call->url, *post = call->post, *type = call->content_type;
        call->url = call->post = call->content_type = NULL;
        SDL_UnlockMutex(ra_calls_lock);

        ra_http_result result;
        ra_http_do(url, post, type, &result);

        SDL_LockMutex(ra_calls_lock);
        for (int i = 0; i < RA_MAX_CALLS; i++) {
            if (ra_calls[i].id == id) {
                ra_calls[i].busy = 0;
                ra_calls[i].ready = 1;
                ra_calls[i].status = result.status;
                ra_calls[i].body = result.body;
                ra_calls[i].body_len = result.len;
                break;
            }
        }
        SDL_CondSignal(ra_calls_cond);
        SDL_UnlockMutex(ra_calls_lock);
        free(url);
        free(post);
        free(type);
    }
    return 0;
}

static void ra_pump(void) {
    if (!ra_client || !ra_pump_lock)
        return;
    SDL_LockMutex(ra_pump_lock);
    for (;;) {
        rc_client_server_callback_t callback = NULL;
        void *data = NULL;
        char *body = NULL;
        size_t body_len = 0;
        int status = 0;

        SDL_LockMutex(ra_calls_lock);
        for (int i = 0; i < RA_MAX_CALLS; i++) {
            if (ra_calls[i].id && ra_calls[i].ready) {
                callback = ra_calls[i].callback;
                data = ra_calls[i].callback_data;
                body = ra_calls[i].body;
                body_len = ra_calls[i].body_len;
                status = ra_calls[i].status;
                memset(&ra_calls[i], 0, sizeof ra_calls[i]);
                break;
            }
        }
        SDL_UnlockMutex(ra_calls_lock);

        if (!callback)
            break;
        rc_api_server_response_t response;
        response.body = body;
        response.body_length = body_len;
        response.http_status_code = status;
        callback(&response, data);
        free(body);
    }
    SDL_UnlockMutex(ra_pump_lock);
}

static void ra_server_call(const rc_api_request_t *request, rc_client_server_callback_t callback,
                           void *callback_data, rc_client_t *client) {
    (void)client;
    if (!callback)
        return;
    if (!ra_calls_lock) {
        rc_api_server_response_t response = {NULL, 0, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR};
        callback(&response, callback_data);
        return;
    }

    SDL_LockMutex(ra_calls_lock);
    int slot = -1;
    for (int i = 0; i < RA_MAX_CALLS; i++) {
        if (!ra_calls[i].id) {
            slot = i;
            break;
        }
    }
    int id = 0;
    if (slot >= 0) {
        id = ra_next_id++;
        if (ra_next_id <= 0)
            ra_next_id = 1;
        ra_calls[slot].id = id;
        ra_calls[slot].callback = callback;
        ra_calls[slot].callback_data = callback_data;
        ra_calls[slot].busy = ra_calls[slot].ready = 0;
        ra_calls[slot].status = 0;
        ra_calls[slot].body = NULL;
        ra_calls[slot].body_len = 0;
        ra_calls[slot].url = ra_strdup(request->url);
        ra_calls[slot].post = ra_strdup(request->post_data);
        ra_calls[slot].content_type = ra_strdup(request->content_type);
    }
    SDL_CondSignal(ra_calls_cond);
    SDL_UnlockMutex(ra_calls_lock);

    if (!id) {
        rc_api_server_response_t response = {NULL, 0, RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR};
        callback(&response, callback_data);
    }
}

static uint32_t ra_read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client) {
    (void)client;
    return ra_read ? ra_read(address, buffer, num_bytes, ra_read_ctx) : 0;
}

static void ra_log(const char *message, const rc_client_t *client) {
    (void)client;
    if (getenv("POCKET_RA_LOG") && message)
        fprintf(stderr, "RetroAchievements: %s\n", message);
}

// --- menu -------------------------------------------------------------------

static char ra_rows[PD_RA_MAX_ROWS][RA_ROW_LEN];
static int ra_row_kind[PD_RA_MAX_ROWS];
static uint32_t ra_row_achievement[PD_RA_MAX_ROWS];
static int ra_row_count;
static bool ra_menu_dirty = true;

static void ra_row(int kind, const char *fmt, ...) {
    if (ra_row_count >= PD_RA_MAX_ROWS)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ra_rows[ra_row_count], RA_ROW_LEN, fmt, ap);
    va_end(ap);
    ra_row_kind[ra_row_count] = kind;
    ra_row_achievement[ra_row_count] = 0;
    ra_row_count++;
}

static void ra_summary(void) {
    if (!ra_client || !rc_client_is_game_loaded(ra_client)) {
        ra_total = ra_unlocked = ra_points = ra_points_unlocked = 0;
        return;
    }
    rc_client_user_game_summary_t summary;
    memset(&summary, 0, sizeof summary);
    rc_client_get_user_game_summary(ra_client, &summary);
    ra_total = summary.num_core_achievements;
    ra_unlocked = summary.num_unlocked_achievements;
    ra_points = summary.points_core;
    ra_points_unlocked = summary.points_unlocked;
}

static void ra_menu_build(void) {
    ra_row_count = 0;
    ra_menu_dirty = false;

    if (!ra_client) {
        ra_row(PD_RA_ROW_INFO, "RETROACHIEVEMENTS UNAVAILABLE");
        return;
    }

    const rc_client_user_t *user = rc_client_get_user_info(ra_client);
    bool signed_in = user || (ra_user[0] && ra_token[0]);
    if (signed_in)
        ra_row(PD_RA_ROW_INFO, "SIGNED IN  %s", ra_display[0] ? ra_display : ra_user);
    else
        ra_row(PD_RA_ROW_SIGN_IN, "SIGN IN");
    ra_row(PD_RA_ROW_NOTIFICATIONS, "NOTIFICATIONS  %s", ra_notify ? "ON" : "OFF");
    if (signed_in)
        ra_row(PD_RA_ROW_SIGN_OUT, "SIGN OUT");

    if (ra_signing_in)
        ra_row(PD_RA_ROW_INFO, "SIGNING IN...");

    ra_summary();
    if (ra_game_loading)
        ra_row(PD_RA_ROW_INFO, "LOOKING UP THIS ROM...");
    else if (ra_game_waiting_login)
        ra_row(PD_RA_ROW_INFO, "SIGN IN TO LOAD ACHIEVEMENTS");
    else if (rc_client_is_game_loaded(ra_client) && ra_game_title[0]) {
        ra_row(PD_RA_ROW_INFO, "GAME  %s", ra_game_title);
        ra_row(PD_RA_ROW_INFO, "PROGRESS  %u/%u  %u/%u PTS", ra_unlocked, ra_total, ra_points_unlocked, ra_points);
        if (!ra_total)
            ra_row(PD_RA_ROW_INFO, "NO ACHIEVEMENTS FOR THIS ROM");
    } else if (ra_last_error[0]) {
        ra_row(PD_RA_ROW_INFO, "ERROR  %s", ra_last_error);
    } else {
        ra_row(PD_RA_ROW_INFO, "NO GAME LOADED");
    }

    if (!ra_client || !rc_client_has_achievements(ra_client))
        return;

    rc_client_achievement_list_t *list = rc_client_create_achievement_list(
        ra_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list)
        return;
    for (uint32_t b = 0; b < list->num_buckets && ra_row_count < PD_RA_MAX_ROWS; b++) {
        const rc_client_achievement_bucket_t *bucket = &list->buckets[b];
        for (uint32_t i = 0; i < bucket->num_achievements && ra_row_count < PD_RA_MAX_ROWS; i++) {
            const rc_client_achievement_t *a = bucket->achievements[i];
            // rc_client's own placeholders (a client the server doesn't know).
            if (a->id >= 101000001u)
                continue;
            bool unlocked = a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED || a->unlocked != 0;
            ra_row(PD_RA_ROW_ACHIEVEMENT, "[%c] %s  %u", unlocked ? 'X' : ' ', a->title ? a->title : "", a->points);
            ra_row_kind[ra_row_count - 1] = PD_RA_ROW_ACHIEVEMENT;
            ra_row_achievement[ra_row_count - 1] = a->id;
        }
    }
    rc_client_destroy_achievement_list(list);
}

int pd_ra_menu_count(void) {
    if (ra_menu_dirty)
        ra_menu_build();
    return ra_row_count;
}

const char *pd_ra_menu_row(int row) {
    if (ra_menu_dirty)
        ra_menu_build();
    return row >= 0 && row < ra_row_count ? ra_rows[row] : "";
}

int pd_ra_menu_action(int row) {
    if (ra_menu_dirty)
        ra_menu_build();
    return row >= 0 && row < ra_row_count ? ra_row_kind[row] : PD_RA_ROW_INFO;
}

void pd_ra_menu_detail(int row, char *out, size_t size) {
    if (!out || !size)
        return;
    out[0] = 0;
    if (ra_menu_dirty)
        ra_menu_build();
    if (row < 0 || row >= ra_row_count)
        return;
    if (ra_row_kind[row] == PD_RA_ROW_ACHIEVEMENT && ra_client) {
        const rc_client_achievement_t *a = rc_client_get_achievement_info(ra_client, ra_row_achievement[row]);
        if (a) {
            snprintf(out, size, "%s (%u) - %s", a->title ? a->title : "", a->points,
                     a->description ? a->description : "");
            return;
        }
    }
    if (ra_row_kind[row] == PD_RA_ROW_INFO && ra_client) {
        const rc_client_user_t *u = rc_client_get_user_info(ra_client);
        if (u && row == 0) {
            snprintf(out, size, "%s  SCORE %u  SOFTCORE %u", u->display_name ? u->display_name : ra_user,
                     u->score, u->score_softcore);
            return;
        }
    }
    snprintf(out, size, "%s", ra_rows[row]);
}

void pd_ra_menu_open(void) {
    ra_menu_dirty = true;
    ra_menu_build();
}

void pd_ra_sign_out(void) {
    if (ra_client) {
        if (rc_client_is_game_loaded(ra_client))
            rc_client_unload_game(ra_client);
        rc_client_logout(ra_client);
    }
    ra_user[0] = ra_display[0] = ra_token[0] = 0;
    ra_game_loading = false;
    ra_game_waiting_login = ra_game_active && ra_hash[0];
    ra_game_title[0] = 0;
    ra_game_id = 0;
    ra_total = ra_unlocked = ra_points = ra_points_unlocked = 0;
    ra_last_error[0] = 0;
    ra_settings_save();
    ra_menu_dirty = true;
    if (ra_notify)
        ra_push_popup("Signed out of RetroAchievements");
}

void pd_ra_toggle_notifications(void) {
    ra_notify = !ra_notify;
    ra_settings_save();
    ra_menu_dirty = true;
}

// --- login / game callbacks -------------------------------------------------

static void ra_game_loaded_callback(int result, const char *error_message, rc_client_t *client, void *userdata);

static void ra_login_callback(int result, const char *error_message, rc_client_t *client, void *userdata) {
    (void)userdata;
    ra_signing_in = false;
    if (!client) {
        snprintf(ra_last_error, sizeof ra_last_error, "%s", error_message ? error_message : "Sign in failed");
        ra_menu_dirty = true;
        return;
    }
    const rc_client_user_t *u = result == RC_OK ? rc_client_get_user_info(client) : NULL;
    if (u) {
        snprintf(ra_user, sizeof ra_user, "%s", u->username ? u->username : "");
        snprintf(ra_display, sizeof ra_display, "%s", u->display_name ? u->display_name : ra_user);
        snprintf(ra_token, sizeof ra_token, "%s", u->token ? u->token : "");
        ra_last_error[0] = 0;
        ra_settings_save();
        ra_game_waiting_login = false;
        ra_push_popup("Signed in as %s", ra_display[0] ? ra_display : ra_user);
        // Signed in after the game started: load its set now.
        if (ra_game_active && ra_hash[0] && !rc_client_is_game_loaded(client) && !ra_game_loading) {
            ra_game_loading = true;
            rc_client_begin_load_game(client, ra_hash, ra_game_loaded_callback, NULL);
        }
    } else {
        snprintf(ra_last_error, sizeof ra_last_error, "%s", error_message ? error_message : "Sign in failed");
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN) {
            ra_token[0] = 0;
            ra_settings_save();
        }
        if (ra_game_active && ra_hash[0])
            ra_game_waiting_login = true;
        ra_push_popup("RetroAchievements: %s", ra_last_error);
    }
    ra_menu_dirty = true;
}

static void ra_game_loaded_callback(int result, const char *error_message, rc_client_t *client, void *userdata) {
    (void)userdata;
    if (!ra_game_active)
        return;
    ra_game_loading = false;
    ra_game_waiting_login = false;
    ra_game_title[0] = 0;
    ra_last_error[0] = 0;
    if (result == RC_OK) {
        const rc_client_game_t *g = rc_client_get_game_info(client);
        if (g) {
            snprintf(ra_game_title, sizeof ra_game_title, "%s", g->title ? g->title : "");
            ra_game_id = g->id;
        }
        ra_summary();
        if (ra_total)
            ra_push_popup("RetroAchievements: %s  %u/%u", ra_game_title, ra_unlocked, ra_total);
        else
            ra_push_popup("RetroAchievements: no achievements for this ROM");
    } else {
        snprintf(ra_last_error, sizeof ra_last_error, "%s", error_message ? error_message : "Couldn't load the set");
        ra_push_popup("RetroAchievements: %s", ra_last_error);
    }
    ra_menu_dirty = true;
}

static void ra_event(const rc_client_event_t *event, rc_client_t *client) {
    (void)client;
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        if (event->achievement)
            ra_push_popup("Achievement: %s (%u)", event->achievement->title ? event->achievement->title : "",
                          event->achievement->points);
        ra_menu_dirty = true;
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        ra_push_popup("Game complete: %s", ra_game_title[0] ? ra_game_title : "all achievements earned");
        ra_menu_dirty = true;
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        if (event->server_error && event->server_error->error_message) {
            snprintf(ra_last_error, sizeof ra_last_error, "%s", event->server_error->error_message);
            ra_push_popup("RetroAchievements: %s", ra_last_error);
            ra_menu_dirty = true;
        }
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        if (!ra_offline_shown) {
            ra_offline_shown = true;
            ra_push_popup("RetroAchievements: offline, unlocks will retry");
        }
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        if (ra_offline_shown) {
            ra_offline_shown = false;
            ra_push_popup("RetroAchievements: back online");
        }
        break;
    default:
        break;
    }
}

// --- the prompt -------------------------------------------------------------

static int ra_prompt_kind;
static char ra_prompt_user[RA_USER_LEN];
static char ra_prompt_pass[RA_TOKEN_LEN];
static char ra_prompt_value[RA_TOKEN_LEN + 16];

int pd_ra_prompt(void) {
    return ra_prompt_kind;
}

const char *pd_ra_prompt_title(void) {
    return ra_prompt_kind == PD_RA_PROMPT_PASSWORD ? "PASSWORD" : "USERNAME";
}

const char *pd_ra_prompt_value(void) {
    if (ra_prompt_kind == PD_RA_PROMPT_PASSWORD) {
        if (!ra_prompt_pass[0])
            return "(type a password)";
        size_t n = strlen(ra_prompt_pass), o = 0;
        for (size_t i = 0; i < n && o + 1 < sizeof ra_prompt_value; i++)
            if (((unsigned char)ra_prompt_pass[i] & 0xC0) != 0x80)
                ra_prompt_value[o++] = '*';
        ra_prompt_value[o] = 0;
        return ra_prompt_value;
    }
    return ra_prompt_user[0] ? ra_prompt_user : "(type a username)";
}

static char *ra_prompt_buffer(void) {
    return ra_prompt_kind == PD_RA_PROMPT_PASSWORD ? ra_prompt_pass : ra_prompt_user;
}

static size_t ra_prompt_capacity(void) {
    return ra_prompt_kind == PD_RA_PROMPT_PASSWORD ? sizeof ra_prompt_pass : sizeof ra_prompt_user;
}

void pd_ra_prompt_text(const char *utf8) {
    if (!utf8 || ra_prompt_kind == PD_RA_PROMPT_NONE)
        return;
    char *buf = ra_prompt_buffer();
    size_t len = strlen(buf), cap = ra_prompt_capacity();
    for (const char *p = utf8; *p && len + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c != 0x7F)
            buf[len++] = (char)c;
    }
    buf[len] = 0;
}

void pd_ra_prompt_backspace(void) {
    if (ra_prompt_kind == PD_RA_PROMPT_NONE)
        return;
    char *buf = ra_prompt_buffer();
    size_t len = strlen(buf);
    if (!len)
        return;
    do {
        len--;
    } while (len && ((unsigned char)buf[len] & 0xC0) == 0x80);
    buf[len] = 0;
}

bool pd_ra_prompt_submit(void) {
    if (ra_prompt_kind == PD_RA_PROMPT_USERNAME) {
        if (!ra_prompt_user[0])
            return false;
        ra_prompt_kind = PD_RA_PROMPT_PASSWORD;
        ra_prompt_pass[0] = 0;
        return false;
    }
    if (ra_prompt_kind == PD_RA_PROMPT_PASSWORD) {
        if (!ra_prompt_pass[0] || !ra_client) {
            ra_prompt_kind = PD_RA_PROMPT_NONE;
            return true;
        }
        ra_signing_in = true;
        ra_last_error[0] = 0;
        rc_client_begin_login_with_password(ra_client, ra_prompt_user, ra_prompt_pass, ra_login_callback, NULL);
        memset(ra_prompt_pass, 0, sizeof ra_prompt_pass);
        ra_prompt_kind = PD_RA_PROMPT_NONE;
        ra_menu_dirty = true;
        return true;
    }
    return true;
}

void pd_ra_prompt_cancel(void) {
    if (ra_prompt_kind == PD_RA_PROMPT_PASSWORD)
        memset(ra_prompt_pass, 0, sizeof ra_prompt_pass);
    ra_prompt_kind = PD_RA_PROMPT_NONE;
}

void pd_ra_sign_in(void) {
    if (!ra_client || ra_signing_in || rc_client_get_user_info(ra_client))
        return;
    ra_prompt_kind = PD_RA_PROMPT_USERNAME;
    ra_prompt_user[0] = 0;
    ra_prompt_pass[0] = 0;
}

// --- lifecycle --------------------------------------------------------------

void pd_ra_init(const char *dataDir, const char *version, pd_ra_read_memory_fn read, void *readCtx) {
    if (ra_ready)
        return;
    ra_read = read;
    ra_read_ctx = readCtx;
    snprintf(ra_data_dir, sizeof ra_data_dir, "%s", dataDir ? dataDir : ".");
    ra_settings_load();

    ra_client = rc_client_create(ra_read_memory, ra_server_call);
    if (!ra_client)
        return;
    char clause[64] = "";
    rc_client_get_user_agent_clause(ra_client, clause, sizeof clause);
    snprintf(ra_user_agent, sizeof ra_user_agent, "PokeDaisy/%s (Windows) %s", version ? version : "dev", clause);
    if (!clause[0])
        snprintf(ra_user_agent, sizeof ra_user_agent, "PokeDaisy/%s (Windows)", version ? version : "dev");

    int log_level = getenv("POCKET_RA_LOG") ? RC_CLIENT_LOG_LEVEL_INFO : RC_CLIENT_LOG_LEVEL_WARN;
    rc_client_enable_logging(ra_client, log_level, ra_log);
    rc_client_set_event_handler(ra_client, ra_event);
    // Softcore: save states and rewind are allowed, as on Android.
    rc_client_set_hardcore_enabled(ra_client, 0);

    ra_calls_lock = SDL_CreateMutex();
    ra_pump_lock = SDL_CreateMutex();
    ra_calls_cond = SDL_CreateCond();
    if (ra_calls_lock && ra_pump_lock && ra_calls_cond) {
        ra_http_thread = SDL_CreateThread(ra_http_thread_fn, "pokedaisy-ra-http", NULL);
        if (!ra_http_thread) {
            SDL_DestroyCond(ra_calls_cond);
            ra_calls_cond = NULL;
        }
    }

    ra_ready = true;
    if (ra_user[0] && ra_token[0])
        rc_client_begin_login_with_token(ra_client, ra_user, ra_token, ra_login_callback, NULL);
}

void pd_ra_shutdown(void) {
    if (!ra_client)
        return;
    if (ra_client && rc_client_is_game_loaded(ra_client))
        rc_client_unload_game(ra_client);
    if (ra_http_thread) {
        SDL_LockMutex(ra_calls_lock);
        ra_http_quit = 1;
        SDL_CondSignal(ra_calls_cond);
        SDL_UnlockMutex(ra_calls_lock);
        int status = 0;
        SDL_WaitThread(ra_http_thread, &status);
        ra_http_thread = NULL;
    }
    for (int i = 0; i < RA_MAX_CALLS; i++) {
        free(ra_calls[i].url);
        free(ra_calls[i].post);
        free(ra_calls[i].content_type);
        free(ra_calls[i].body);
    }
    memset(ra_calls, 0, sizeof ra_calls);
    if (ra_calls_cond)
        SDL_DestroyCond(ra_calls_cond);
    if (ra_calls_lock)
        SDL_DestroyMutex(ra_calls_lock);
    if (ra_pump_lock)
        SDL_DestroyMutex(ra_pump_lock);
    ra_calls_cond = NULL;
    ra_calls_lock = ra_pump_lock = NULL;
    rc_client_destroy(ra_client);
    ra_client = NULL;
    ra_ready = false;
}

void pd_ra_game_start(int console, const char *romPath) {
    if (!ra_client)
        return;
    ra_game_active = true;
    ra_hash[0] = 0;
    ra_game_loading = false;
    ra_game_waiting_login = false;
    ra_game_title[0] = 0;
    ra_game_id = 0;
    ra_total = ra_unlocked = ra_points = ra_points_unlocked = 0;
    ra_last_error[0] = 0;
    ra_menu_dirty = true;

    if (console <= 0 || !romPath) {
        ra_game_active = false;
        snprintf(ra_last_error, sizeof ra_last_error, "Not available for this game");
        return;
    }

    if (!rc_hash_generate_from_file(ra_hash, (uint32_t)console, romPath) || !ra_hash[0]) {
        ra_game_active = false;
        snprintf(ra_last_error, sizeof ra_last_error, "Couldn't hash this ROM");
        return;
    }
    if (!rc_client_get_user_info(ra_client)) {
        // The set loads from the login callback once the account is in.
        ra_game_waiting_login = true;
        return;
    }
    ra_game_loading = true;
    rc_client_begin_load_game(ra_client, ra_hash, ra_game_loaded_callback, NULL);
}

void pd_ra_game_stop(void) {
    if (!ra_client)
        return;
    rc_client_unload_game(ra_client);
    ra_game_active = false;
    ra_hash[0] = 0;
    ra_game_loading = false;
    ra_game_waiting_login = false;
    ra_game_title[0] = 0;
    ra_game_id = 0;
    ra_total = ra_unlocked = ra_points = ra_points_unlocked = 0;
    ra_last_error[0] = 0;
    ra_menu_dirty = true;
}

void pd_ra_frame(bool paused) {
    if (!ra_client)
        return;
    ra_pump();
    if (!rc_client_is_game_loaded(ra_client))
        return;
    if (paused)
        rc_client_idle(ra_client);
    else
        rc_client_do_frame(ra_client);
}

void pd_ra_pump(void) {
    if (ra_client)
        ra_pump();
}

void pd_ra_save_progress(const char *statePath) {
    if (!ra_client || !statePath || !rc_client_is_game_loaded(ra_client))
        return;
    size_t size = rc_client_progress_size(ra_client);
    if (!size)
        return;
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data)
        return;
    char path[1400];
    snprintf(path, sizeof path, "%s.ra", statePath);
    if (rc_client_serialize_progress_sized(ra_client, data, size) == RC_OK) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(data, 1, size, f);
            fclose(f);
        }
    }
    free(data);
}

void pd_ra_load_progress(const char *statePath) {
    if (!ra_client || !statePath || !rc_client_is_game_loaded(ra_client))
        return;
    char path[1400];
    snprintf(path, sizeof path, "%s.ra", statePath);
    FILE *f = fopen(path, "rb");
    if (!f) {
        rc_client_deserialize_progress_sized(ra_client, NULL, 0);
        ra_menu_dirty = true;
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        uint8_t *data = (uint8_t *)malloc((size_t)n);
        if (data && fread(data, 1, (size_t)n, f) == (size_t)n)
            rc_client_deserialize_progress_sized(ra_client, data, (size_t)n);
        free(data);
    }
    fclose(f);
    ra_menu_dirty = true;
}
