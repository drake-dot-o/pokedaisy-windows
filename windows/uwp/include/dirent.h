/* dirent.h for MSVC: the host lists its save backups and the ROM folders with opendir / readdir / closedir, and
 * Windows spells that FindFirstFileA. Only d_name and d_type are ever read. */
#ifndef PD_UWP_DIRENT_H
#define PD_UWP_DIRENT_H

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define DT_UNKNOWN 0
#define DT_DIR 1
#define DT_REG 2

struct dirent {
  char d_name[MAX_PATH];
  unsigned char d_type;
};

typedef struct DIR {
  HANDLE find;
  WIN32_FIND_DATAA data;
  int first;
  int done;
  struct dirent ent;
} DIR;

static inline DIR *opendir(const char *path) {
  char pattern[MAX_PATH + 4];
  DIR *d;
  snprintf(pattern, sizeof pattern, "%s\\*", path);
  d = (DIR *)calloc(1, sizeof *d);
  if (!d)
    return NULL;
  d->find = FindFirstFileA(pattern, &d->data);
  if (d->find == INVALID_HANDLE_VALUE) {
    free(d);
    return NULL;
  }
  d->first = 1;
  return d;
}

static inline struct dirent *readdir(DIR *d) {
  if (!d || d->done)
    return NULL;
  if (!d->first && !FindNextFileA(d->find, &d->data)) {
    d->done = 1;
    return NULL;
  }
  d->first = 0;
  snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", d->data.cFileName);
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
