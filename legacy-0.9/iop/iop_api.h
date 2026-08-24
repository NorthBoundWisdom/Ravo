/*
    This file is part of darktable,
    Copyright (C) 2016-2023 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/module_api.h"
#include "common/colorspaces.h"

#ifdef FULL_API_H

#include "common/introspection.h"

#include <cairo/cairo.h>
#include <gtk/gtk.h>
#include <glib.h>
#include <stdint.h>

#ifdef HAVE_OPENCL
#include <CL/cl.h>
#endif

#if defined(__cplusplus) && !defined(INCLUDE_API_FROM_MODULE_H)
extern "C"
{
#endif

    struct dt_iop_module_so_t;
    struct dt_iop_module_t;
    struct dt_dev_pixelpipe_t;
    struct dt_dev_pixelpipe_iop_t;
    struct dt_iop_roi_t;
    struct dt_develop_tiling_t;
    struct dt_iop_buffer_dsc_t;
    struct _GtkWidget;

#ifndef DT_IOP_PARAMS_T
#define DT_IOP_PARAMS_T
    typedef void dt_iop_params_t;
#endif

    /* early definition of modules to do type checking */

#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif

#endif // FULL_API_H


    /** this initializes static, hardcoded presets for this module and is
 * called only once per run of dt. */
    DT_MODULE_API_OPTIONAL(void, init_presets, struct dt_iop_module_so_t *self);
    /** called once per module, at startup. */
    DT_MODULE_API_OPTIONAL(void, init_global, struct dt_iop_module_so_t *self);
    /** called once per module, at shutdown. */
    DT_MODULE_API_OPTIONAL(void, cleanup_global, struct dt_iop_module_so_t *self);

    /** get name of the module, to be translated. */
    DT_MODULE_API_REQUIRED(const char *, name, void);
    /** get the alternative names or keywords of the module, to be
 * translated. Separate variants by a pipe | */
    DT_MODULE_API_DEFAULT(const char *, aliases, void);
    /** get the default group this module belongs to. */
    DT_MODULE_API_DEFAULT(int, default_group, void);
    /** get the iop module flags. */
    DT_MODULE_API_DEFAULT(int, flags, void);

    /** get a descriptive text used for example in a tooltip in more modules */
    DT_MODULE_API_DEFAULT(const char **, description, struct dt_iop_module_t *self);

    DT_MODULE_API_DEFAULT(int, operation_tags, void);
    DT_MODULE_API_DEFAULT(int, operation_tags_filter, void);

    /** what do the iop want as an input? */
    DT_MODULE_API_DEFAULT(void, input_format, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
            struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);
    /** what will it output? */
    DT_MODULE_API_DEFAULT(void, output_format, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
            struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_buffer_dsc_t *dsc);

    /** what default colorspace this iop use? */
    DT_MODULE_API_REQUIRED(dt_iop_colorspace_type_t, default_colorspace, struct dt_iop_module_t *self,
             struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
    /** what input colorspace it expects? */
    DT_MODULE_API_DEFAULT(dt_iop_colorspace_type_t, input_colorspace, struct dt_iop_module_t *self,
            struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
    /** what will it output? */
    DT_MODULE_API_DEFAULT(dt_iop_colorspace_type_t, output_colorspace, struct dt_iop_module_t *self,
            struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
    /** what colorspace the blend module operates with? */
    DT_MODULE_API_DEFAULT(dt_iop_colorspace_type_t, blend_colorspace, struct dt_iop_module_t *self,
            struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);

    /** report back info for tiling: memory usage and overlap. Memory
 * usage: factor * input_size + overhead */
    DT_MODULE_API_DEFAULT(void, tiling_callback, struct dt_iop_module_t *self,
            struct dt_dev_pixelpipe_iop_t *piece, const struct dt_iop_roi_t *roi_in,
            const struct dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling);

    /** callback methods for gui. */
    /** synch gtk interface with gui params, if necessary. */
    DT_MODULE_API_OPTIONAL(void, gui_update, struct dt_iop_module_t *self);
    /** reset ui to defaults */
    DT_MODULE_API_OPTIONAL(void, gui_reset, struct dt_iop_module_t *self);
    /** construct widget. */
    DT_MODULE_API_OPTIONAL(void, gui_init, struct dt_iop_module_t *self);
    /** apply color picker results */
    DT_MODULE_API_OPTIONAL(void, color_picker_apply, struct dt_iop_module_t *self, struct _GtkWidget *picker,
             struct dt_dev_pixelpipe_t *pipe);
    /** called by standard widget callbacks after value changed */
    DT_MODULE_API_OPTIONAL(void, gui_changed, struct dt_iop_module_t *self, GtkWidget *widget, void *previous);
    /** destroy widget. */
    DT_MODULE_API_OPTIONAL(void, gui_cleanup, struct dt_iop_module_t *self);
    /** optional method called after darkroom expose. */
    DT_MODULE_API_OPTIONAL(void, gui_post_expose, struct dt_iop_module_t *self, cairo_t *cr, float width,
             float height, float pointerx, float pointery, float zoom_scale);
    /** optional callback to be notified if the module acquires gui focus/loses it. */
    DT_MODULE_API_OPTIONAL(void, gui_focus, struct dt_iop_module_t *self, gboolean in);

    /** Key accelerator registration callbacks */
    DT_MODULE_API_OPTIONAL(GSList *, mouse_actions, struct dt_iop_module_t *self);

    /** optional event callbacks */
    DT_MODULE_API_OPTIONAL(int, mouse_leave, struct dt_iop_module_t *self);
    DT_MODULE_API_OPTIONAL(int, mouse_moved, struct dt_iop_module_t *self, float x, float y, double pressure,
             int which, float zoom_scale);
    DT_MODULE_API_OPTIONAL(int, button_released, struct dt_iop_module_t *self, float x, float y, int which,
             uint32_t state, float zoom_scale);
    DT_MODULE_API_OPTIONAL(int, button_pressed, struct dt_iop_module_t *self, float x, float y, double pressure,
             int which, int type, uint32_t state, float zoom_scale);
    DT_MODULE_API_OPTIONAL(int, scrolled, struct dt_iop_module_t *self, float x, float y, int up, uint32_t state);

    DT_MODULE_API_OPTIONAL(void, init, struct dt_iop_module_t *self); // this MUST set params_size!
    DT_MODULE_API_DEFAULT(void, cleanup, struct dt_iop_module_t *self);

    /** this inits the piece of the pipe, allocing piece->data as necessary. */
    DT_MODULE_API_DEFAULT(void, init_pipe, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
            struct dt_dev_pixelpipe_iop_t *piece);
    /** this resets the params to factory defaults. used at the beginning
 * of each history synch. */
    /** this commits (a mutex will be locked to synch pipe/gui) the given
 * history params to the pixelpipe piece.
 */
    DT_MODULE_API_DEFAULT(void, commit_params, struct dt_iop_module_t *self, dt_iop_params_t *params,
            struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece);
    /** this is the chance to update default parameters, after the full raw is loaded. */
    DT_MODULE_API_OPTIONAL(void, reload_defaults, struct dt_iop_module_t *self);
    /** called after the image has changed in darkroom */
    DT_MODULE_API_OPTIONAL(void, change_image, struct dt_iop_module_t *self);

    /** this destroys all resources needed by the piece of the pixelpipe. */
    DT_MODULE_API_DEFAULT(void, cleanup_pipe, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_t *pipe,
            struct dt_dev_pixelpipe_iop_t *piece);
    /*
  modify_roi_in and modify_roi_out are used inside the pixelpipe to calculate regions of interest (roi).

  2nd pass: which roi would this operation need as input to fill the given output region?
  Called while preparing a pixelpipe for processing traversing all modules from last to first!
  Initial roi_in is what we got by the 1st pass via dt_dev_pixelpipe_get_dimensions

  Modules requiring data from somewhere else it's roi_out will have to make sure those data
  can be accessed.
  Examples: crop, retouch, lens for morphing modules,
    highlights, cacorrect or finalscale for modules that might enforce the full pixelpipe

  Be aware that the tiling code also makes use of this to calculate the roi of a tile.
*/
    DT_MODULE_API_OPTIONAL(void, modify_roi_in, struct dt_iop_module_t *self,
             struct dt_dev_pixelpipe_iop_t *piece, const struct dt_iop_roi_t *roi_out,
             struct dt_iop_roi_t *roi_in);
    /*
  1st pass: how large would the output be, given this input roi?

  Used in dt_dev_pixelpipe_get_dimensions() traversing all active modules
  in the pipe from first to last module calculating the final dimension.

  This is always called with the full buffer before processing for the first tested module.
  For all active modules without the function declared the roi_out will be the same as it's roi_in.
  Examples: rawprepare, crop
*/
    DT_MODULE_API_OPTIONAL(void, modify_roi_out, struct dt_iop_module_t *self,
             struct dt_dev_pixelpipe_iop_t *piece, struct dt_iop_roi_t *roi_out,
             const struct dt_iop_roi_t *roi_in);
    // allow to select a shape inside an iop
    DT_MODULE_API_OPTIONAL(void, masks_selection_changed, struct dt_iop_module_t *self,
             const int form_selected_id);

    /** this is the temp homebrew callback to operations.
  * x,y, and scale are just given for orientation in the framebuffer. i and o are
  * scaled to the same size width*height and contain a max of 3 floats. other color
  * formats may be filled by this callback, if the pipeline can handle it. */
    /** the simplest variant of process(). you can only use OpenMP SIMD here, no intrinsics */
    /** must be provided by each IOP. */
    DT_MODULE_API_REQUIRED(void, process, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
             const void *const i, void *const o, const struct dt_iop_roi_t *const roi_in,
             const struct dt_iop_roi_t *const roi_out);
    /** a tiling variant of process(). */
    DT_MODULE_API_DEFAULT(void, process_tiling, struct dt_iop_module_t *self,
            struct dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
            const struct dt_iop_roi_t *const roi_in, const struct dt_iop_roi_t *const roi_out,
            const int bpp);

#ifdef HAVE_OPENCL
    /** the opencl equivalent of process().
 *   Both process_xx_cl() functions return a CL error code with CL_SUCCESS signalling ok.
 *   Please note: until 4.4 this int was in fact used as a gboolean
 *   with TRUE set if the function worked fine.
*/
    DT_MODULE_API_OPTIONAL(int, process_cl, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
             cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *const roi_in,
             const struct dt_iop_roi_t *const roi_out);
    /** a tiling variant of process_cl(). */
    DT_MODULE_API_DEFAULT(int, process_tiling_cl, struct dt_iop_module_t *self,
            struct dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
            const struct dt_iop_roi_t *const roi_in, const struct dt_iop_roi_t *const roi_out,
            const int bpp);
#endif

    /** this functions are used for distort iop
 * points is an array of float {x1,y1,x2,y2,...}
 * size is 2*points_count */
    /** points before the iop is applied => point after processed */
    DT_MODULE_API_OPTIONAL(gboolean, distort_transform, struct dt_iop_module_t *self,
             struct dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count);
    /** reverse points after the iop is applied => point before process */
    DT_MODULE_API_OPTIONAL(gboolean, distort_backtransform, struct dt_iop_module_t *self,
             struct dt_dev_pixelpipe_iop_t *piece, float *points, size_t points_count);
    DT_MODULE_API_OPTIONAL(void, distort_mask, struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
             const float *const in, float *const out, const struct dt_iop_roi_t *const roi_in,
             const struct dt_iop_roi_t *const roi_out);

    // introspection related callbacks, will be auto-implemented if
    // DT_MODULE_INTROSPECTION() is used,
    DT_MODULE_API_OPTIONAL(int, introspection_init, struct dt_iop_module_so_t *self, int api_version);
    DT_MODULE_API_DEFAULT(dt_introspection_t *, get_introspection, void);
    DT_MODULE_API_DEFAULT(dt_introspection_field_t *, get_introspection_linear, void);
    DT_MODULE_API_DEFAULT(void *, get_p, const void *param, const char *name);
    DT_MODULE_API_DEFAULT(dt_introspection_field_t *, get_f, const char *name);

    // optional preference entry to add at the bottom of the preset menu
    DT_MODULE_API_OPTIONAL(void, set_preferences, void *menu, struct dt_iop_module_t *self);

#ifdef FULL_API_H

#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

#if defined(__cplusplus) && !defined(INCLUDE_API_FROM_MODULE_H)
} // extern "C"
#endif

#endif // FULL_API_H
