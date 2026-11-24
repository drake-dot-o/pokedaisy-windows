/* dlfcn.h for a Windows build: the host loads a core's .dll the way it dlopens a .so, and finds its own folder
 * without /proc/self/exe. A packaged (UWP) build loads the core the way an app container must, LoadPackagedLibrary,
 * out of the package's install folder. */
#ifndef PD_WIN32_DL_H
#define PD_WIN32_DL_H

#ifndef _WIN32
#error "win32_dl.h is the Windows stand-in for dlfcn.h"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define RTLD_NOW 0
#define RTLD_LOCAL 0

#ifdef PD_UWP
#include <appmodel.h>

/* An app container has no access to the loader's ordinary search: LoadPackagedLibrary is the one that loads a module
 * the package holds, and it takes the module's file name, not a path (it looks in the package's install folder). The
 * host names the core by its full path, so only the last part is handed over. */
static inline void *dlopen(const char *path, int mode) {
  const char *name = path, *p;
  wchar_t wide[256];
  (void)mode;
  for (p = path; *p; p++)
    if (*p == '/' || *p == '\\')
      name = p + 1;
  if (MultiByteToWideChar(CP_ACP, 0, name, -1, wide, (int)(sizeof wide / sizeof wide[0])) <= 0)
    return NULL;
  return (void *)LoadPackagedLibrary(wide, 0);
}

/* A packaged app's install folder (Windows.ApplicationModel.Package's InstalledLocation, as plain Win32 spells it):
 * where the exe and the cores sit. -1 when the process has no package. */
static inline long pd_install_dir(char *buf, size_t size) {
  WCHAR wide[1024];
  UINT32 len = (UINT32)(sizeof wide / sizeof wide[0]);
  int n;
  if (GetCurrentPackagePath(&len, wide) != ERROR_SUCCESS)
    return -1;
  n = WideCharToMultiByte(CP_ACP, 0, wide, -1, buf, (int)size, NULL, NULL);
  return n > 0 ? (long)(n - 1) : -1;
}
#else
static inline void *dlopen(const char *path, int mode) {
  (void)mode;
  return (void *)LoadLibraryA(path);
}
#endif
static inline void *dlsym(void *handle, const char *name) {
  return (void *)(uintptr_t)GetProcAddress((HMODULE)handle, name);
}
static inline int dlclose(void *handle) {
  return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
static inline char *dlerror(void) {
  static char text[256];
  DWORD code = GetLastError();
  if (!code) {
    *text = 0;
    return text;
  }
  if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, text, sizeof text,
                      NULL))
    snprintf(text, sizeof text, "Windows error %lu", (unsigned long)code);
  return text;
}

/* This executable's full path (readlink("/proc/self/exe") on Linux); -1 when it can't be read. */
static inline long pd_exe_path(char *buf, size_t size) {
  DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
  return n && n < size ? (long)n : -1;
}

#endif
