/*
    This file is part of darktable,
    Copyright (C) 2020-2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

// DarkTableNext requires C11 and C++17 atomics for inter-thread signalling.
#ifdef __cplusplus

#include <atomic>

typedef std::atomic<int> dt_atomic_int;
inline void dt_atomic_set_int(dt_atomic_int *var, int value)
{
    std::atomic_store(var, value);
}
inline int dt_atomic_get_int(const dt_atomic_int *var)
{
    return std::atomic_load(var);
}
inline int dt_atomic_add_int(dt_atomic_int *var, int incr)
{
    return std::atomic_fetch_add(var, incr);
}
inline int dt_atomic_sub_int(dt_atomic_int *var, int decr)
{
    return std::atomic_fetch_sub(var, decr);
}
inline int dt_atomic_exch_int(dt_atomic_int *var, int value)
{
    return std::atomic_exchange(var, value);
}
inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value)
{
    return std::atomic_compare_exchange_strong(var, expected, value);
}
inline void dt_atomic_incr_int(dt_atomic_int *var)
{
    std::atomic_fetch_add(var, 1);
}
inline void dt_atomic_decr_int(dt_atomic_int *var)
{
    std::atomic_fetch_sub(var, 1);
}

#else

#include <stdatomic.h>

typedef atomic_int dt_atomic_int;
inline void dt_atomic_set_int(dt_atomic_int *var, int value)
{
    atomic_store(var, value);
}
inline int dt_atomic_get_int(const dt_atomic_int *var)
{
    return atomic_load(var);
}
inline int dt_atomic_add_int(dt_atomic_int *var, int incr)
{
    return atomic_fetch_add(var, incr);
}
inline int dt_atomic_sub_int(dt_atomic_int *var, int decr)
{
    return atomic_fetch_sub(var, decr);
}
inline int dt_atomic_exch_int(dt_atomic_int *var, int value)
{
    return atomic_exchange(var, value);
}
inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value)
{
    return atomic_compare_exchange_strong(var, expected, value);
}
inline void dt_atomic_incr_int(dt_atomic_int *var)
{
    atomic_fetch_add(var, 1);
}
inline void dt_atomic_decr_int(dt_atomic_int *var)
{
    atomic_fetch_sub(var, 1);
}

#endif

inline int dt_atomic_incr_int_if_zero(dt_atomic_int *var)
{
    int expected = 0;
    return dt_atomic_CAS_int(var, &expected, 1);
}
