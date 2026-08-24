// CMake uses config.cmake.h to generate config.h within the build folder.
#pragma once

#include "build_runtime_config.h"
#include "common/darktable_api.h"

#include <stddef.h>

// clang-format off
// it butchers @@ and ${} :(

#define PACKAGE_NAME "@CMAKE_PROJECT_NAME@"
#define PACKAGE_URL "https://github.com/NorthBoundWisdom/DarkTableNext"
#define PACKAGE_BUGREPORT PACKAGE_URL "/issues"
#define PACKAGE_DOCS PACKAGE_URL

// These are defined in build/bin/version_gen.c, which is compiled as C.
#ifdef __cplusplus
extern "C"
{
#endif
extern DT_CORE_API const char darktable_package_version[];
extern DT_CORE_API const char darktable_package_string[];
extern DT_CORE_API const char darktable_last_commit_year[];
extern DT_CORE_API const char *const dt_supported_extensions[];
#ifdef __cplusplus
}
#endif

#define DT_SUPPORTED_EXTENSIONS_INITIALIZER {"@DT_SUPPORTED_EXTENSIONS_STRING@", NULL}

#cmakedefine DARKTABLE_LIBDIR    "@REL_BIN_TO_LIBDIR@"
#cmakedefine DARKTABLE_DATADIR   "@REL_BIN_TO_DATADIR@"
#cmakedefine DARKTABLE_SHAREDIR  "@REL_BIN_TO_SHAREDIR@"

#define SHARED_MODULE_PREFIX "@CMAKE_SHARED_MODULE_PREFIX@"
#define SHARED_MODULE_SUFFIX "@CMAKE_SHARED_MODULE_SUFFIX@"

#define WANTED_STACK_SIZE (@WANTED_STACK_SIZE@)
#define WANTED_THREADS_STACK_SIZE (@WANTED_THREADS_STACK_SIZE@)

// clang-format on

#if defined(_OPENMP)
#define SIMD() simd
#else
#define SIMD()
#endif

// see http://clang.llvm.org/docs/LanguageExtensions.html
#ifndef __has_feature      // Optional of course.
#define __has_feature(x) 0 // Compatibility with non-clang compilers.
#endif
#ifndef __has_extension
#define __has_extension __has_feature // Compatibility with pre-3.0 compilers.
#endif

// see https://github.com/google/sanitizers/wiki/AddressSanitizerManualPoisoning
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) ((void)(addr), (void)(size))
#endif

#cmakedefine HAVE_CPUID_H 1
#cmakedefine HAVE___GET_CPUID 1

#cmakedefine HAVE_THREAD_RWLOCK_ARCH_T_READERS 1

#cmakedefine HAVE_THREAD_RWLOCK_ARCH_T_NR_READERS 1

/******************************************************************************
 * OpenCL target settings
 * OpenCL 1.2 is the version supported by Apple, otherwise we use 3.0
 *****************************************************************************/
#ifdef HAVE_APPLE_KEYCHAIN
#define CL_TARGET_OPENCL_VERSION 120
#else
#define CL_TARGET_OPENCL_VERSION 300
#endif
