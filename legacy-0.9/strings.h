#pragma once

#if defined(_MSC_VER)

#include <string.h>

#define strcasecmp _stricmp
#define strncasecmp _strnicmp

#else

#include_next <strings.h>

#endif
