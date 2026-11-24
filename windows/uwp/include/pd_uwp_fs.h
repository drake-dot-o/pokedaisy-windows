/* File access for the UWP host. A packaged app can't reach an outside folder with the CRT's fopen / stat: those only
 * see the package and LocalState. The ...FromApp Win32 calls can, once broadFileSystemAccess is in the manifest, and
 * they work on Xbox Dev Mode too (FolderPicker does not). Every host file goes through this header, so a ROM folder or
 * save folder on another drive behaves like one in LocalState. */
#ifndef PD_UWP_FS_H
#define PD_UWP_FS_H

#include <windows.h>
#include <fileapifromapp.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

/* The host's paths are UTF-8 and use '/'; the Win32 calls take UTF-16 and accept either slash once it is a backslash. */
static inline int pd_uwp_wide(const char *path, wchar_t *out, int count) {
  if (!path || !*path || !out || count <= 0)
    return 0;
  int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, out, count);
  if (n <= 0)
    return 0;
  for (int i = 0; out[i]; i++)
    if (out[i] == L'/')
      out[i] = L'\\';
  return n;
}

static inline FILE *pd_uwp_fopen(const char *path, const char *mode) {
  if (!path || !mode)
    return NULL;
  const bool plus = strchr(mode, '+') != NULL;
  const bool truncate = strchr(mode, 'w') != NULL;
  const bool append = strchr(mode, 'a') != NULL;
  DWORD access = (plus || truncate || append) ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  DWORD disposition = truncate ? CREATE_ALWAYS : append ? OPEN_ALWAYS : OPEN_EXISTING;
  wchar_t wide[PATH_MAX * 2];
  if (!pd_uwp_wide(path, wide, (int)(sizeof wide / sizeof *wide)))
    return NULL;
  HANDLE h = CreateFileFromAppW(wide, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disposition,
                                FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return NULL;
  int flags = _O_BINARY | (access & GENERIC_WRITE ? _O_RDWR : _O_RDONLY);
  if (append)
    flags |= _O_APPEND;
  int fd = _open_osfhandle((intptr_t)h, flags);
  if (fd < 0) {
    CloseHandle(h);
    return NULL;
  }
  FILE *f = _fdopen(fd, mode);
  if (!f) {
    _close(fd);
    return NULL;
  }
  return f;
}

static inline int pd_uwp_stat(const char *path, struct stat *st) {
  if (!path || !st) {
    errno = EINVAL;
    return -1;
  }
  wchar_t wide[PATH_MAX * 2];
  if (!pd_uwp_wide(path, wide, (int)(sizeof wide / sizeof *wide))) {
    errno = ENOENT;
    return -1;
  }
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExFromAppW(wide, GetFileExInfoStandard, &data)) {
    errno = ENOENT;
    return -1;
  }
  memset(st, 0, sizeof *st);
  st->st_mode = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? (_S_IFDIR | 0755) : (_S_IFREG | 0644);
  ULARGE_INTEGER size, stamp;
  size.LowPart = data.nFileSizeLow;
  size.HighPart = data.nFileSizeHigh;
  st->st_size = (off_t)size.QuadPart;
  stamp.LowPart = data.ftLastWriteTime.dwLowDateTime;
  stamp.HighPart = data.ftLastWriteTime.dwHighDateTime;
  st->st_mtime = (time_t)(stamp.QuadPart / 10000000ULL - 11644473600ULL);
  return 0;
}

static inline int pd_uwp_access(const char *path, int mode) {
  (void)mode;
  struct stat st;
  return pd_uwp_stat(path, &st);
}

static inline int pd_uwp_mkdir(const char *path) {
  if (!path || !*path)
    return -1;
  wchar_t wide[PATH_MAX * 2];
  if (!pd_uwp_wide(path, wide, (int)(sizeof wide / sizeof *wide)))
    return -1;
  if (CreateDirectoryFromAppW(wide, NULL))
    return 0;
  if (GetLastError() == ERROR_ALREADY_EXISTS)
    return 0;
  errno = EACCES;
  return -1;
}

static inline int pd_uwp_unlink(const char *path) {
  wchar_t wide[PATH_MAX * 2];
  if (!pd_uwp_wide(path, wide, (int)(sizeof wide / sizeof *wide)))
    return -1;
  if (DeleteFileFromAppW(wide))
    return 0;
  errno = EACCES;
  return -1;
}

static inline int pd_uwp_rename(const char *from, const char *to) {
  wchar_t wideFrom[PATH_MAX * 2], wideTo[PATH_MAX * 2];
  if (!pd_uwp_wide(from, wideFrom, (int)(sizeof wideFrom / sizeof *wideFrom)) ||
      !pd_uwp_wide(to, wideTo, (int)(sizeof wideTo / sizeof *wideTo)))
    return -1;
  if (ReplaceFileFromAppW(wideTo, wideFrom, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL))
    return 0;
  if (MoveFileFromAppW(wideFrom, wideTo))
    return 0;
  // MoveFileFromAppW won't put a file over one that is there on every file system: replace it by hand, as pd_rename did
  // on desktop Windows. A crash between the two leaves no save, so the host writes a .tmp and renames only once it is whole.
  if (DeleteFileFromAppW(wideTo) && MoveFileFromAppW(wideFrom, wideTo))
    return 0;
  errno = EACCES;
  return -1;
}

/* windows.h's legacy far / near are empty macros: the 3ds core has a variable called far. */
#ifdef far
#undef far
#endif
#ifdef near
#undef near
#endif
#ifdef small
#undef small
#endif

/* The host's fopen / stat / access / ... reach an outside drive through the ...FromApp calls above. */
#define fopen(path, mode) pd_uwp_fopen((path), (mode))
#define stat(path, st) pd_uwp_stat((path), (st))
#define access(path, mode) pd_uwp_access((path), (mode))
#define unlink(path) pd_uwp_unlink((path))
#define remove(path) pd_uwp_unlink((path))
#define rename(from, to) pd_uwp_rename((from), (to))
#define mkdir(path, mode) pd_uwp_mkdir((path))

#endif