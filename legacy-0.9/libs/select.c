/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <gdk/gdkkeysyms.h>
#include "libs/lib_api.h"

DT_MODULE(1)

const char *name(dt_lib_module_t *self)
{
    return _("selection");
}

const char *description(dt_lib_module_t *self)
{
    return _("modify which of the displayed\n"
             "images are selected");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
    return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
    return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_select_t
{
    GtkWidget *select_all_button;
    GtkWidget *select_none_button;
    GtkWidget *select_invert_button;
    GtkWidget *select_film_roll_button;
    GtkWidget *select_untouched_button;
} dt_lib_select_t;

void gui_update(dt_lib_module_t *self)
{
    dt_lib_select_t *d = self->data;

    const uint32_t collection_cnt = dt_collection_get_count_no_group(darktable.collection);
    const uint32_t selected_cnt = dt_collection_get_selected_count();

    gtk_widget_set_sensitive(GTK_WIDGET(d->select_all_button), selected_cnt < collection_cnt);
    gtk_widget_set_sensitive(GTK_WIDGET(d->select_none_button), selected_cnt > 0);

    gtk_widget_set_sensitive(GTK_WIDGET(d->select_invert_button), collection_cnt > 0);

    //theoretically can count if there are unaltered in collection but no need to waste CPU cycles on that.
    gtk_widget_set_sensitive(GTK_WIDGET(d->select_untouched_button), collection_cnt > 0);

    gtk_widget_set_sensitive(GTK_WIDGET(d->select_film_roll_button), selected_cnt > 0);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
    dt_lib_gui_queue_update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs,
                                         int next, dt_lib_module_t *self)
{
    dt_lib_gui_queue_update(self);
}

static void button_clicked(GtkWidget *widget, gpointer user_data)
{
    switch (GPOINTER_TO_INT(user_data))
    {
    case 0: // all
        dt_selection_select_all(darktable.selection);
        break;
    case 1: // none
        dt_selection_clear(darktable.selection);
        break;
    case 2: // invert
        dt_selection_invert(darktable.selection);
        break;
    case 4: // untouched
        dt_selection_select_unaltered(darktable.selection);
        break;
    default: // case 3: same film roll
        dt_selection_select_filmroll(darktable.selection);
    }

    dt_control_queue_redraw_center();
}

int position(const dt_lib_module_t *self)
{
    return 800;
}

void gui_init(dt_lib_module_t *self)
{
    dt_lib_select_t *d = malloc(sizeof(dt_lib_select_t));
    self->data = d;
    self->widget = gtk_grid_new();

    GtkGrid *grid = GTK_GRID(self->widget);
    gtk_grid_set_column_homogeneous(grid, TRUE);
    int line = 0;

    d->select_all_button = dt_action_button_new(
        self, N_("select all"), button_clicked, GINT_TO_POINTER(0),
        _("select all images in current collection"), GDK_KEY_a, GDK_CONTROL_MASK);
    gtk_grid_attach(grid, d->select_all_button, 0, line, 1, 1);

    d->select_none_button =
        dt_action_button_new(self, N_("select none"), button_clicked, GINT_TO_POINTER(1),
                             _("clear selection"), GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    gtk_grid_attach(grid, d->select_none_button, 1, line++, 1, 1);

    d->select_invert_button = dt_action_button_new(
        self, N_("invert selection"), button_clicked, GINT_TO_POINTER(2),
        _("select unselected images\nin current collection"), GDK_KEY_i, GDK_CONTROL_MASK);
    gtk_grid_attach(grid, d->select_invert_button, 0, line, 1, 1);

    d->select_film_roll_button = dt_action_button_new(
        self, N_("select film roll"), button_clicked, GINT_TO_POINTER(3),
        _("select all images which are in the same\nfilm roll as the selected images"), 0, 0);
    gtk_grid_attach(grid, d->select_film_roll_button, 1, line++, 1, 1);

    d->select_untouched_button =
        dt_action_button_new(self, N_("select untouched"), button_clicked, GINT_TO_POINTER(4),
                             _("select untouched images in\ncurrent collection"), 0, 0);
    gtk_grid_attach(grid, d->select_untouched_button, 0, line, 2, 1);

    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->select_all_button))),
                            PANGO_ELLIPSIZE_START);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->select_none_button))),
                            PANGO_ELLIPSIZE_START);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->select_film_roll_button))),
                            PANGO_ELLIPSIZE_START);

    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_SELECTION_CHANGED, _image_selection_changed_callback);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_COLLECTION_CHANGED, _collection_updated_callback);
}

void gui_cleanup(dt_lib_module_t *self)
{
    free(self->data);
    self->data = NULL;
}
