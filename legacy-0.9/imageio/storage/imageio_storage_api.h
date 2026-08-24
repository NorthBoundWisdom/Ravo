/*
    This file is part of darktable,
    Copyright (C) 2016-2025 darktable developers.

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

#include "common/darktable.h"
#include "common/module_api.h"

#ifdef FULL_API_H

G_BEGIN_DECLS

#include <glib.h>
#include <stdint.h>

struct dt_imageio_module_storage_t;
struct dt_imageio_module_format_t;
struct dt_imageio_module_data_t;
struct dt_export_metadata_t;
enum dt_colorspaces_color_profile_type_t;
enum dt_iop_color_intent_t;

/* early definition of modules to do type checking */

#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif

#endif // FULL_API_H


/* get translated module name */
DT_MODULE_API_REQUIRED(const char *, name, const struct dt_imageio_module_storage_t *self);
/* construct widget above */
DT_MODULE_API_REQUIRED(void, gui_init, struct dt_imageio_module_storage_t *self);
/* destroy resources */
DT_MODULE_API_REQUIRED(void, gui_cleanup, struct dt_imageio_module_storage_t *self);
/* reset options to defaults */
DT_MODULE_API_REQUIRED(void, gui_reset, struct dt_imageio_module_storage_t *self);
/* allow the module to initialize itself */
DT_MODULE_API_REQUIRED(void, init, struct dt_imageio_module_storage_t *self);
/* try and see if this format is supported? */
DT_MODULE_API_DEFAULT(gboolean, supported, struct dt_imageio_module_storage_t *self,
        struct dt_imageio_module_format_t *format);
/* get storage max supported image dimension, return 0 if no dimension restrictions exists. */
DT_MODULE_API_OPTIONAL(int, dimension, struct dt_imageio_module_storage_t *self,
         struct dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height);
/* get storage recommended image dimension, return 0 if no recommendation exists. */
DT_MODULE_API_OPTIONAL(int, recommended_dimension, struct dt_imageio_module_storage_t *self,
         struct dt_imageio_module_data_t *data, uint32_t *width, uint32_t *height);

/* called once at the beginning (before exporting image), if implemented
   * can change the list of exported images (including a NULL list)
 */
DT_MODULE_API_OPTIONAL(int, initialize_store, struct dt_imageio_module_storage_t *self,
         struct dt_imageio_module_data_t *data, struct dt_imageio_module_format_t **format,
         struct dt_imageio_module_data_t **fdata, GList **images, const gboolean high_quality,
         const gboolean upscale);
/* this actually does the work */
DT_MODULE_API_REQUIRED(int, store, struct dt_imageio_module_storage_t *self,
         struct dt_imageio_module_data_t *self_data, const dt_imgid_t imgid,
         struct dt_imageio_module_format_t *format, struct dt_imageio_module_data_t *fdata,
         const int num, const int total, const gboolean high_quality, const gboolean upscale,
         const gboolean is_scaling, const double scale_factor, const gboolean export_masks,
         const enum dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
         enum dt_iop_color_intent_t icc_intent, struct dt_export_metadata_t *metadata);
/* called once at the end (after exporting all images), if implemented. */
DT_MODULE_API_OPTIONAL(void, finalize_store, struct dt_imageio_module_storage_t *self,
         struct dt_imageio_module_data_t *data);

DT_MODULE_API_REQUIRED(size_t, params_size, struct dt_imageio_module_storage_t *self);
DT_MODULE_API_REQUIRED(void *, get_params, struct dt_imageio_module_storage_t *self);
DT_MODULE_API_REQUIRED(void, free_params, struct dt_imageio_module_storage_t *self,
         struct dt_imageio_module_data_t *data);
DT_MODULE_API_REQUIRED(int, set_params, struct dt_imageio_module_storage_t *self, const void *params,
         const int size);

DT_MODULE_API_OPTIONAL(void, export_dispatched, struct dt_imageio_module_storage_t *self);

DT_MODULE_API_OPTIONAL(char *, ask_user_confirmation, struct dt_imageio_module_storage_t *self);

/* ask the storage if export is currently possible */
DT_MODULE_API_OPTIONAL(gboolean, export_enabled, struct dt_imageio_module_storage_t *self);

/* for storage modules which require a login */
DT_MODULE_API_OPTIONAL(gboolean, storage_login, struct dt_imageio_module_storage_t *self);

#ifdef FULL_API_H

#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

G_END_DECLS

#endif // FULL_API_H
