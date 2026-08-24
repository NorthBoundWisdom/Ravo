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

#include "common/atomic.h"

extern inline void dt_atomic_set_int(dt_atomic_int *var, int value);
extern inline int dt_atomic_get_int(const dt_atomic_int *var);
extern inline int dt_atomic_add_int(dt_atomic_int *var, int incr);
extern inline int dt_atomic_sub_int(dt_atomic_int *var, int decr);
extern inline int dt_atomic_exch_int(dt_atomic_int *var, int value);
extern inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value);
extern void dt_atomic_incr_int(dt_atomic_int *var);
extern void dt_atomic_decr_int(dt_atomic_int *var);
extern int dt_atomic_incr_int_if_zero(dt_atomic_int *var);
