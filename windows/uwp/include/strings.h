/* strings.h for MSVC: the case-blind comparisons glibc keeps here, and the CRT spells _stricmp. */
#ifndef PD_UWP_STRINGS_H
#define PD_UWP_STRINGS_H

#include <string.h>

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

#endif
