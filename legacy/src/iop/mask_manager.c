/*
    This file is part of darktable,
    Copyright (C) 2018-2023 darktable developers.

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

/*
 * This is a dummy module intended only to be used in history so hist->module is not NULL
 * when the entry correspond to the mask manager
 *
 * It is always disabled and do not show in module list, only in history
 *
 * We start at version 2 so previous version of dt can add records in history with NULL params
 */

#include "common/imagebuf.h"
#include "develop/develop.h"

DT_MODULE_INTROSPECTION(2, dt_iop_mask_manager_params_t)

typedef struct dt_iop_mask_manager_params_t
{
    int dummy;
} dt_iop_mask_manager_params_t;

typedef struct dt_iop_mask_manager_params_t dt_iop_mask_manager_data_t;

const char *name()
{
    return _("mask manager");
}

int groups()
{
    return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
    return IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_UNSAFE_COPY;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
    return IOP_CS_RGB;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i,
             void *const o, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
    const int ch = piece->colors;
    dt_iop_image_copy_by_size(o, i, roi_out->width, roi_out->height, ch);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
               cl_mem dev_out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
    const int devid = piece->pipe->devid;
    const int width = roi_in->width;
    const int height = roi_in->height;

    const size_t region[2] = {width, height};
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, CLIMG_ORIGIN, CLIMG_ORIGIN, region);
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
    memcpy(piece->data, params, sizeof(dt_iop_mask_manager_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
    piece->data = malloc(sizeof(dt_iop_mask_manager_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
    free(piece->data);
    piece->data = NULL;
}
