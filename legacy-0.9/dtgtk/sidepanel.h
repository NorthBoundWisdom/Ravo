/*
    This file is part of darktable,
    Copyright (C) 2015-2020 darktable developers.

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

#include <glib-object.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_SIDE_PANEL dtgtk_side_panel_get_type()
G_DECLARE_FINAL_TYPE(GtkDarktableSidePanel, dtgtk_side_panel, DTGTK, SIDE_PANEL, GtkBox)

struct _GtkDarktableSidePanel
{
    GtkBox panel;
};

GtkWidget *dtgtk_side_panel_new();

G_END_DECLS
