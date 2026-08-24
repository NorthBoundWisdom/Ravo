/*
    This file is part of darktable,
    Copyright (C) 2016-2026 darktable developers.

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
#include <glib.h>
#include <gtk/gtk.h>

#ifdef FULL_API_H

G_BEGIN_DECLS

#include <glib.h>
#include <cairo/cairo.h>
#include <stddef.h>
#include <stdint.h>

struct dt_lib_module_t;
struct dt_view_t;

/* early definition of modules to do type checking */

#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif

#endif // FULL_API_H


/** get name of the module, to be translated. */
DT_MODULE_API_REQUIRED(const char *, name, struct dt_lib_module_t *self);

/** does the module support a preset label? */
DT_MODULE_API_DEFAULT(gboolean, has_preset_label, struct dt_lib_module_t *self);

/** get the views which the module should be loaded in. */
DT_MODULE_API_REQUIRED(enum dt_view_type_flags_t, views, struct dt_lib_module_t *self);
/** get the container which the module should be placed in */
DT_MODULE_API_REQUIRED(uint32_t, container, struct dt_lib_module_t *self);
/** check if module should use a expander or not, default implementation
    will make the module expandable and storing the expanding state,
    if not the module will always be shown without the expander. */
DT_MODULE_API_DEFAULT(gboolean, expandable, struct dt_lib_module_t *self);

/** constructor */
DT_MODULE_API_OPTIONAL(void, init, struct dt_lib_module_t *self);
/** callback methods for gui. */
/** get a description string to be used as tooltip on the module header */
DT_MODULE_API_OPTIONAL(const char *, description, struct dt_lib_module_t *self);
/** construct widget. */
DT_MODULE_API_REQUIRED(void, gui_init, struct dt_lib_module_t *self);
/** destroy widget. */
DT_MODULE_API_REQUIRED(void, gui_cleanup, struct dt_lib_module_t *self);
/** reset to defaults. */
DT_MODULE_API_OPTIONAL(void, gui_reset, struct dt_lib_module_t *self);
/** update libs gui when visible
    triggered by dt_lib_gui_queue_update.
    don't use for widgets accessible via actions when hidden. */
DT_MODULE_API_DEFAULT(void, gui_update, struct dt_lib_module_t *self);

DT_MODULE_API_OPTIONAL(GtkWidget *, gui_tool_box, struct dt_lib_module_t *self);

/** entering a view, only called if lib is displayed on the new view */
DT_MODULE_API_OPTIONAL(void, view_enter, struct dt_lib_module_t *self, struct dt_view_t *old_view,
         struct dt_view_t *new_view);
/** entering a view, only called if lib is displayed on the old view */
DT_MODULE_API_OPTIONAL(void, view_leave, struct dt_lib_module_t *self, struct dt_view_t *old_view,
         struct dt_view_t *new_view);

/** optional event callbacks for big center widget. */
/** optional method called after lighttable expose. */
DT_MODULE_API_OPTIONAL(void, gui_post_expose, struct dt_lib_module_t *self, cairo_t *cr, int32_t width,
         int32_t height, int32_t pointerx, int32_t pointery);
/** mouse_leave called when mouse is leaving the center canvas */
DT_MODULE_API_OPTIONAL(int, mouse_leave, struct dt_lib_module_t *self);
DT_MODULE_API_OPTIONAL(int, mouse_moved, struct dt_lib_module_t *self, double x, double y, double pressure,
         int which);
DT_MODULE_API_OPTIONAL(int, button_released, struct dt_lib_module_t *self, double x, double y, int which,
         uint32_t state);
DT_MODULE_API_OPTIONAL(int, button_pressed, struct dt_lib_module_t *self, double x, double y, double pressure,
         int which, int type, uint32_t state);
DT_MODULE_API_OPTIONAL(int, scrolled, struct dt_lib_module_t *self, double x, double y, int up);
DT_MODULE_API_OPTIONAL(int, position, const struct dt_lib_module_t *self);

/** implement these two if you want customizable presets to be stored in db. */
DT_MODULE_API_OPTIONAL(void *, get_params, struct dt_lib_module_t *self, int *size);
DT_MODULE_API_OPTIONAL(int, set_params, struct dt_lib_module_t *self, const void *params, int size);
DT_MODULE_API_OPTIONAL(void, init_presets, struct dt_lib_module_t *self);
DT_MODULE_API_OPTIONAL(void, manage_presets, struct dt_lib_module_t *self);
DT_MODULE_API_OPTIONAL(void, set_preferences, void *menu, struct dt_lib_module_t *self);
/** check if the module can autoapply presets. Default is FALSE */
DT_MODULE_API_DEFAULT(gboolean, preset_autoapply, struct dt_lib_module_t *self);

#ifdef FULL_API_H

#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

G_END_DECLS

#endif // FULL_API_H
