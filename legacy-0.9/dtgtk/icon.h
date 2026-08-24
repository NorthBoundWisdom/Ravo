/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include "paint.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_ICON dtgtk_icon_get_type()
G_DECLARE_FINAL_TYPE(GtkDarktableIcon, dtgtk_icon, DTGTK, ICON, GtkEventBox)

struct _GtkDarktableIcon
{
    GtkEventBox widget;
    DTGTKCairoPaintIconFunc icon;
    gint icon_flags;
    void *icon_data;
};

/** instantiate a new darktable icon control passing paint function as content */
GtkWidget *dtgtk_icon_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata);

/** set the paint function for a icon */
void dtgtk_icon_set_paint(GtkWidget *icon, DTGTKCairoPaintIconFunc paint, gint paintflags,
                          void *paintdata);

G_END_DECLS
