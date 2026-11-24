/* What MSVC's CRT leaves out of the SDL host's POSIX odds and ends, for the UWP build of linux/main.c. Forced in with
 * /FI, as windows/include/pd_win_compat.h is for the MinGW one. The only 3ds edit for MSVC is the small guard in
 * pd_ui.h and pd_font.h, where rpcndr.h's legacy small macro would otherwise rewrite the parameter names. */
#ifndef PD_UWP_COMPAT_H
#define PD_UWP_COMPAT_H

#if !defined(_WIN32) || !defined(PD_UWP)
#error "pd_uwp_compat.h is the UWP build's compat header"
#endif

#include <limits.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The host's files, through the ...FromApp calls a packaged app needs for a folder outside the package. This also
 * supplies the fopen / stat / access / unlink / rename / mkdir macros for every host source. */
#include "pd_uwp_fs.h"

#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

/* The POSIX names the CRT only offers with an underscore (strtok_r is strtok_s, the save pointer last). */
#define strdup _strdup
#define strtok_r strtok_s
#define fileno _fileno
#ifndef alloca
#define alloca _alloca
#endif

/* Time broken down the POSIX way (Windows spells it _s, the arguments the other way round). */
static inline struct tm *pd_uwp_localtime_r(const time_t *t, struct tm *out) {
  return localtime_s(out, t) ? NULL : out;
}
static inline struct tm *pd_uwp_gmtime_r(const time_t *t, struct tm *out) {
  return gmtime_s(out, t) ? NULL : out;
}
#define localtime_r pd_uwp_localtime_r
#define gmtime_r pd_uwp_gmtime_r

#endif
