/*
    This file is part of darktable,
    Copyright (C) 2021-2023 darktable developers.

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

#include "common/dttypes.h"

// inverts the given un-padded 3x3 matrix
int mat3inv(float *const dst, const float *const src);

// inverts the given padded 3x3 matrix
int mat3SSEinv(dt_colormatrix_t dst, const dt_colormatrix_t src);
