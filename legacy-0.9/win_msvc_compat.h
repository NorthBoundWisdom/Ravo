#pragma once

#if defined(_MSC_VER)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <BaseTsd.h>
#include <errno.h>
#include <fcntl.h>
#include <intrin.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

// The retained core and the pinned RawSpeed source root use GNU attributes
// for diagnostics and optimization hints. MSVC has no equivalent spelling for
// most of them; removing the hints preserves the program's ABI and behavior.
#ifndef __attribute__
#define __attribute__(...)
#endif

#ifndef __thread
#define __thread __declspec(thread)
#endif

#ifndef __restrict__
#define __restrict__ __restrict
#endif

#ifndef __builtin_unreachable
#define __builtin_unreachable() __assume(0)
#endif

#ifndef __PRETTY_FUNCTION__
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#ifndef __builtin_expect
#define __builtin_expect(expression, expected) (expression)
#endif

#ifndef __cplusplus
#ifndef __builtin_assume_aligned
#define __builtin_assume_aligned(pointer, alignment) (pointer)
#endif
#endif

#ifndef __builtin_prefetch
#define __builtin_prefetch(address, ...) ((void)(address))
#endif

static inline int dt_msvc_builtin_clz(const unsigned int value)
{
  unsigned long most_significant_bit = 0;
  return _BitScanReverse(&most_significant_bit, value) ? 31 - (int)most_significant_bit : 32;
}

#ifndef __builtin_clz
#define __builtin_clz(value) dt_msvc_builtin_clz((unsigned int)(value))
#endif

static inline long dt_msvc_sync_fetch_and_add(volatile long *value, const long addend)
{
  return _InterlockedExchangeAdd(value, addend);
}

#ifndef __sync_fetch_and_add
#define __sync_fetch_and_add(value, addend) dt_msvc_sync_fetch_and_add((volatile long *)(value), (long)(addend))
#endif

#ifndef __cplusplus
static inline ssize_t getline(char **line, size_t *capacity, FILE *stream)
{
  if(!line || !capacity || !stream)
  {
    errno = EINVAL;
    return -1;
  }

  if(!*line || *capacity == 0)
  {
    *capacity = 128;
    *line = malloc(*capacity);
    if(!*line)
      return -1;
  }

  size_t length = 0;
  int character = EOF;
  while((character = fgetc(stream)) != EOF)
  {
    if(length + 1 >= *capacity)
    {
      if(*capacity > (size_t)-1 / 2)
      {
        errno = ENOMEM;
        return -1;
      }

      const size_t expanded_capacity = *capacity * 2;
      char *const expanded_line = realloc(*line, expanded_capacity);
      if(!expanded_line)
        return -1;
      *line = expanded_line;
      *capacity = expanded_capacity;
    }

    (*line)[length++] = (char)character;
    if(character == '\n')
      break;
  }

  if(length == 0 && character == EOF)
    return -1;

  (*line)[length] = '\0';
  return (ssize_t)length;
}
#endif

#ifndef M_1_PI
#define M_1_PI 0.31830988618379067154
#endif

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

#ifndef PATH_MAX
#define PATH_MAX 32767
#endif

#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR _O_RDWR
#define O_APPEND _O_APPEND
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_EXCL _O_EXCL
#define O_TEXT _O_TEXT
#define O_BINARY _O_BINARY
#endif

#ifndef R_OK
#define R_OK 4
#endif

#ifndef W_OK
#define W_OK 2
#endif

// Windows has no independent execute-permission bit. Directory callers only
// need to verify that writes are permitted.
#ifndef X_OK
#define X_OK 0
#endif

#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif

static inline struct tm *dt_msvc_localtime_r(const time_t *timep, struct tm *result)
{
  return localtime_s(result, timep) == 0 ? result : 0;
}

#ifndef localtime_r
#define localtime_r(timep, result) dt_msvc_localtime_r((timep), (result))
#endif

#ifdef __cplusplus

#include <limits>
#include <type_traits>

template <typename T>
constexpr bool dt_msvc_add_overflow(T lhs, T rhs, T *result) noexcept
{
  static_assert(std::is_integral_v<T>);

  if constexpr(std::is_signed_v<T>)
  {
    if((rhs > 0 && lhs > std::numeric_limits<T>::max() - rhs)
       || (rhs < 0 && lhs < std::numeric_limits<T>::min() - rhs))
      return true;
  }
  else if(lhs > std::numeric_limits<T>::max() - rhs)
  {
    return true;
  }

  *result = static_cast<T>(lhs + rhs);
  return false;
}

template <typename T>
constexpr bool dt_msvc_mul_overflow(T lhs, T rhs, T *result) noexcept
{
  static_assert(std::is_integral_v<T>);

  if constexpr(std::is_signed_v<T>)
  {
    if((lhs > 0 && ((rhs > 0 && lhs > std::numeric_limits<T>::max() / rhs)
                    || (rhs < 0 && rhs < std::numeric_limits<T>::min() / lhs)))
       || (lhs < 0 && ((rhs > 0 && lhs < std::numeric_limits<T>::min() / rhs)
                    || (rhs < 0 && lhs < std::numeric_limits<T>::max() / rhs))))
      return true;
  }
  else if(rhs != 0 && lhs > std::numeric_limits<T>::max() / rhs)
  {
    return true;
  }

  *result = static_cast<T>(lhs * rhs);
  return false;
}

#ifndef __builtin_sadd_overflow
#define __builtin_sadd_overflow(lhs, rhs, result) dt_msvc_add_overflow((lhs), (rhs), (result))
#endif

#ifndef __builtin_mul_overflow
#define __builtin_mul_overflow(lhs, rhs, result) dt_msvc_mul_overflow((lhs), (rhs), (result))
#endif

#endif
#endif
