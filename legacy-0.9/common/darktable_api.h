/* Windows import/export annotations for the core runtime ABI. */
#pragma once

#ifdef _WIN32
#ifdef DT_LIB_DARKTABLE_BUILD
#define DT_CORE_API __declspec(dllexport)
#else
#define DT_CORE_API __declspec(dllimport)
#endif
#else
#define DT_CORE_API
#endif
