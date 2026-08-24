/*
    This file is part of darktable,
    Copyright (C) 2022-2026 darktable developers.

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

typedef enum dt_distance_transform_t
{
    DT_DISTANCE_TRANSFORM_NONE = 0,
    DT_DISTANCE_TRANSFORM_MASK = 1
} dt_distance_transform_t;

#define DT_DISTANCE_TRANSFORM_MAX (1e20)

float dt_image_distance_transform(const float *const src, float *const out, const size_t width,
                                  const size_t height, const float clip,
                                  const dt_distance_transform_t mode);
