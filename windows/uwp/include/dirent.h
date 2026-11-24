/* dirent.h for the UWP host: the host lists its ROM folders and save backups with opendir / readdir / closedir, and a
 * packaged app reaches an outside folder only through the ...FromApp calls (FindFirstFileA sees LocalState alone). Only
 * d_name and d_type are ever read. */
#ifndef PD_UWP_DIRENT_H
#define PD_UWP_DIRENT_H

#include "pd_uwp_fs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define DT_UNKNOWN 0
#define DT_DIR 1
#define DT_REG 2

struct dirent {
  char d_name[PATH_MAX];
  unsigned char d_type;
};

typedef struct DIR {
  HANDLE find;
  WIN32_FIND_DATAW data;
  bool first;
  bool done;
  struct dirent ent;
} DIR;

static inline DIR *opendir(const char *path) {
  if (!path || !*path)
    return NULL;
  char pattern[PATH_MAX + 4];
  size_t n = strlen(path);
  snprintf(pattern, sizeof pattern, "%s%s*", path, n && path[n - 1] == '/' ? "" : "/");
  wchar_t wide[PATH_MAX * 2];
  if (!pd_uwp_wide(pattern, wide, (int)(sizeof wide / sizeof *wide)))
    return NULL;
  DIR *d = (DIR *)calloc(1, sizeof *d);
  if (!d)
    return NULL;
  d->find = FindFirstFileExFromAppW(wide, FindExInfoBasic, &d->data, FindExSearchNameMatch, NULL, 0);
  if (d->find == INVALID_HANDLE_VALUE) {
    free(d);
    return NULL;
  }
  d->first = true;
  return d;
}

static inline struct dirent *readdir(DIR *d) {
  if (!d || d->done)
    return NULL;
  if (!d->first && !FindNextFileW(d->find, &d->data)) {
    d->done = true;
    return NULL;
  }
  d->first = false;
  if (WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1, d->ent.d_name, (int)sizeof d->ent.d_name, NULL, NULL) <= 0)
    return NULL;
  d->ent.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
  return &d->ent;
}

static inline void closedir(DIR *d) {
  if (!d)
    return;
  FindClose(d->find);
  free(d);
}

#endif
