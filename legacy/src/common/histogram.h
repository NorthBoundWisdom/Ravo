/*
    This file is part of darktable,
    Copyright (C) 2014-2023 darktable developers.

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

#include <stdint.h>

#include "develop/imageop.h"
#include "develop/pixelpipe.h"
#include "common/iop_profile.h"

/*
 * histogram region of interest
 *
 * image is located in (0,     0)      .. (width,           height)
 * but only            (crop_x,crop_y) .. (width-crop_width,height-crop_height)
 * will be sampled
 */
typedef struct dt_histogram_roi_t
{
    int width, height, crop_x, crop_y, crop_right, crop_bottom;
} dt_histogram_roi_t;

// allocates an aligned histogram buffer if needed, callers
// (pixelpipe, exposure, global histogram) must garbage collect this
// buffer via dt_free_align()
void dt_histogram_helper(dt_dev_histogram_collection_params_t *histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats,
                         const dt_iop_colorspace_type_t cst, const dt_iop_colorspace_type_t cst_to,
                         const void *pixel, uint32_t **histogram, uint32_t *histogram_max,
                         const gboolean compensate_middle_grey,
                         const dt_iop_order_iccprofile_info_t *const profile_info);
