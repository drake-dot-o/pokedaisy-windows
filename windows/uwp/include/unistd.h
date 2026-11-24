/* unistd.h for MSVC: the few POSIX names the SDL host uses, spelled the CRT's way. Found through /I, so no source in
 * linux/ is edited for the UWP build. */
#ifndef PD_UWP_UNISTD_H
#define PD_UWP_UNISTD_H

#include <io.h>
#include <process.h>
#include <stdint.h>

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef F_OK
#define F_OK 0
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

/* pd_uwp_fs.h gives access and unlink the ...FromApp calls when it is in (the UWP build always has it): a CRT _access
 * on an outside folder would say the ROM is gone. */
#ifndef PD_UWP_FS_H
#define access _access
#define unlink _unlink
#endif
#define rmdir _rmdir
#define getcwd _getcwd
#define chdir _chdir
#define read _read
#define write _write
#define close _close
#define isatty _isatty
#define getpid _getpid

#endif
