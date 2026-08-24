/*
    This file is part of darktable,
    Copyright (C) 2016-2026 darktable developers.

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

#include "common/iop_profile.h"

typedef enum dt_colorpicker_size_t
{
    DT_COLOR_PICKER_SIZE_POINT = 0,
    DT_COLOR_PICKER_SIZE_BOX,
} dt_colorpicker_size_t;

typedef enum dt_colorpicker_statistic_t
{
    DT_PICK_MEAN = 0,
    DT_PICK_MIN,
    DT_PICK_MAX,
    DT_PICK_N
} dt_colorpicker_statistic_t;

typedef dt_aligned_pixel_t dt_colorpicker_stats_t[DT_PICK_N];

/** Shared sample geometry and options for the active IOP color picker. */
typedef struct dt_colorpicker_sample_t
{
    dt_pickerpoint_t point;
    dt_pickerbox_t box;
    dt_colorpicker_size_t size;
    gboolean denoise;
    gboolean pick_output;
} dt_colorpicker_sample_t;

struct dt_iop_buffer_dsc_t;
struct dt_iop_roi_t;
enum dt_iop_colorspace_type_t;

typedef enum dt_pixelpipe_picker_source_t
{
    PIXELPIPE_PICKER_INPUT = 0,
    PIXELPIPE_PICKER_OUTPUT = 1
} dt_pixelpipe_picker_source_t;

void dt_color_picker_backtransform_box(dt_develop_t *dev, const int num, const float *in,
                                       float *out);
void dt_color_picker_transform_box(dt_develop_t *dev, const int num, const float *in, float *out,
                                   gboolean scale);
gboolean dt_color_picker_box(dt_iop_module_t *module, const dt_iop_roi_t *roi,
                             const dt_colorpicker_sample_t *const sample,
                             dt_pixelpipe_picker_source_t picker_source, int *box);
void dt_color_picker_helper(const struct dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                            const struct dt_iop_roi_t *roi, const int *const box,
                            const gboolean denoise, dt_colorpicker_stats_t pick,
                            const enum dt_iop_colorspace_type_t image_cst,
                            const enum dt_iop_colorspace_type_t picker_cst,
                            const dt_iop_order_iccprofile_info_t *const profile);
