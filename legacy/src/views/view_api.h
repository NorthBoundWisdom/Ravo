/*
    This file is part of darktable,
    Copyright (C) 2016-2021 darktable developers.

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

#ifdef FULL_API_H

G_BEGIN_DECLS

#include <cairo/cairo.h>
#include <glib.h>
#include <stdint.h>

struct dt_view_t;

/* early definition of modules to do type checking */

#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif

#endif // FULL_API_H


DT_MODULE_API_OPTIONAL(const char *, name, const struct dt_view_t *self); // get translatable name
DT_MODULE_API_OPTIONAL(uint32_t, view, const struct dt_view_t *self);     // get the view type
DT_MODULE_API_DEFAULT(uint32_t, flags, );                                 // get flags of the view
DT_MODULE_API_OPTIONAL(void, init, struct dt_view_t *self);               // init *data
DT_MODULE_API_OPTIONAL(void, gui_init,
         struct dt_view_t *self); // create gtk elements, called after libs are created
DT_MODULE_API_OPTIONAL(void, cleanup, struct dt_view_t *self); // cleanup *data
DT_MODULE_API_OPTIONAL(void, expose, struct dt_view_t *self, cairo_t *cr, int32_t width, int32_t height,
         int32_t pointerx,
         int32_t pointery);                            // expose the module (gtk callback)
DT_MODULE_API_OPTIONAL(gboolean, try_enter, struct dt_view_t *self); // test if enter can succeed.
DT_MODULE_API_OPTIONAL(
    void, enter,
    struct dt_view_t *self); // mode entered, this module got focus. return non-null on failure.
DT_MODULE_API_OPTIONAL(void, leave,
         struct dt_view_t *self); // mode left (is called after the new try_enter has succeeded).
DT_MODULE_API_OPTIONAL(void, reset, struct dt_view_t *self); // reset default appearance

// event callbacks:
DT_MODULE_API_OPTIONAL(void, mouse_enter, struct dt_view_t *self);
DT_MODULE_API_OPTIONAL(void, mouse_leave, struct dt_view_t *self);
DT_MODULE_API_OPTIONAL(void, mouse_moved, struct dt_view_t *self, double x, double y, double pressure, int which);

DT_MODULE_API_OPTIONAL(int, button_released, struct dt_view_t *self, double x, double y, int which,
         uint32_t state);
DT_MODULE_API_OPTIONAL(int, button_pressed, struct dt_view_t *self, double x, double y, double pressure,
         int which, int type, uint32_t state);
DT_MODULE_API_OPTIONAL(void, configure, struct dt_view_t *self, int width, int height);
DT_MODULE_API_OPTIONAL(void, scrolled, struct dt_view_t *self, double x, double y, int up,
         int state); // mouse scrolled in view
DT_MODULE_API_OPTIONAL(void, scrollbar_changed, struct dt_view_t *self, double x,
         double y); // scrollbars changed in view
DT_MODULE_API_OPTIONAL(gboolean, gesture_pan, struct dt_view_t *self, double x, double y, double dx, double dy,
         int state);
DT_MODULE_API_OPTIONAL(gboolean, gesture_pinch, struct dt_view_t *self, double x, double y, double dx, double dy,
         int phase, double scale, int state); // x,y are root (screen-absolute) coords

// list of mouse actions
DT_MODULE_API_OPTIONAL(GSList *, mouse_actions, const struct dt_view_t *self);

#ifdef FULL_API_H

#if defined(__GNUC__)
#pragma GCC visibility pop
#endif

G_END_DECLS

#endif // FULL_API_H
