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
#include "common/image_cache.h"
#include "common/undo.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/jobs/control_jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_image_t
{
    GtkWidget *rotate_cw_button, *rotate_ccw_button, *remove_button;
    GtkWidget *delete_button, *create_hdr_button;
    GtkWidget *duplicate_button, *reset_button, *move_button, *copy_button;
    GtkWidget *group_button, *ungroup_button, *cache_button, *uncache_button;
} dt_lib_image_t;

const char *name(dt_lib_module_t *self)
{
    return _("actions on selection");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
    return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
    return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

static void _duplicate_virgin(dt_action_t *action)
{
    dt_control_duplicate_images(TRUE);
}

static void button_clicked(GtkWidget *widget, gpointer user_data)
{
    const int i = GPOINTER_TO_INT(user_data);
    if (i == 0)
        dt_control_remove_images();
    else if (i == 1)
        dt_control_delete_images();
    // else if(i == 2) dt_control_write_sidecar_files();
    else if (i == 3)
        dt_control_duplicate_images(FALSE);
    else if (i == 4)
        dt_control_flip_images(1);
    else if (i == 5)
        dt_control_flip_images(0);
    else if (i == 6)
        dt_control_flip_images(2);
    else if (i == 7)
        dt_control_merge_hdr();
    else if (i == 8)
        dt_control_move_images();
    else if (i == 9)
        dt_control_copy_images();
    else if (i == 10)
        dt_control_group_images();
    else if (i == 11)
        dt_control_ungroup_images();
    else if (i == 12)
        dt_control_set_local_copy_images();
    else if (i == 13)
        dt_control_reset_local_copy_images();
}

void gui_update(dt_lib_module_t *self)
{
    dt_lib_image_t *d = self->data;
    const int nbimgs = dt_act_on_get_images_nb(FALSE, FALSE);

    const gboolean act_on_any = (nbimgs > 0);
    const uint32_t selected_cnt = dt_collection_get_selected_count();

    gtk_widget_set_sensitive(GTK_WIDGET(d->remove_button), act_on_any);
    gtk_widget_set_sensitive(GTK_WIDGET(d->delete_button), act_on_any);

    gtk_widget_set_sensitive(GTK_WIDGET(d->move_button), act_on_any);
    gtk_widget_set_sensitive(GTK_WIDGET(d->copy_button), act_on_any);

    gtk_widget_set_sensitive(GTK_WIDGET(d->create_hdr_button), act_on_any);
    gtk_widget_set_sensitive(GTK_WIDGET(d->duplicate_button), act_on_any);

    gtk_widget_set_sensitive(GTK_WIDGET(d->rotate_ccw_button), act_on_any);
    gtk_widget_set_sensitive(GTK_WIDGET(d->rotate_cw_button), act_on_any);
    gtk_widget_set_sensitive(GTK_WIDGET(d->reset_button), act_on_any);

    gtk_widget_set_sensitive(GTK_WIDGET(d->cache_button), act_on_any);
    gtk_widget_set_sensitive(GTK_WIDGET(d->uncache_button), act_on_any);

    gtk_widget_set_sensitive(GTK_WIDGET(d->group_button), selected_cnt > 1);

    if (nbimgs > 1)
        gtk_widget_set_sensitive(GTK_WIDGET(d->ungroup_button), TRUE);
    else if (!act_on_any)
    {
        // no images to act on!
        gtk_widget_set_sensitive(GTK_WIDGET(d->ungroup_button), FALSE);
    }
    else
    {
        // exact one image to act on
        const dt_imgid_t imgid = dt_act_on_get_main_image();
        if (dt_is_valid_imgid(imgid))
        {
            dt_image_t *img = dt_image_cache_get(imgid, 'r');
            const int img_group_id = img->group_id;
            dt_image_cache_read_release(img);
            sqlite3_stmt *stmt;
            DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                        "SELECT COUNT(id)"
                                        " FROM main.images"
                                        " WHERE group_id = ?1 AND id != ?2",
                                        -1, &stmt, NULL);
            DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img_group_id);
            DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
            if (stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
            {
                const int images_in_grp = sqlite3_column_int(stmt, 0);
                gtk_widget_set_sensitive(GTK_WIDGET(d->ungroup_button), images_in_grp > 0);
            }
            else
                gtk_widget_set_sensitive(GTK_WIDGET(d->ungroup_button), FALSE);
            if (stmt)
                sqlite3_finalize(stmt);
        }
        else
        {
            gtk_widget_set_sensitive(GTK_WIDGET(d->ungroup_button), FALSE);
        }
    }
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
    dt_lib_gui_queue_update(self);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs,
                                         const int next, dt_lib_module_t *self)
{
    dt_lib_gui_queue_update(self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
    dt_lib_gui_queue_update(self);
}

static void _image_preference_changed(gpointer instance, dt_lib_module_t *self)
{
    dt_lib_image_t *d = self->data;
    gboolean trash = dt_conf_get_bool("send_to_trash");
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->delete_button))),
                       trash ? _("delete (trash)") : _("delete"));
    gtk_widget_set_tooltip_text(d->delete_button,
                                trash ? _("physically delete from disk (using trash if possible)") :
                                        _("physically delete from disk immediately"));
}

int position(const dt_lib_module_t *self)
{
    return 700;
}

void gui_init(dt_lib_module_t *self)
{
    dt_lib_image_t *d = malloc(sizeof(dt_lib_image_t));
    self->data = (void *)d;

    self->widget = gtk_grid_new();
    dt_gui_add_help_link(self->widget, "image");

    // images operations
    GtkGrid *grid = GTK_GRID(self->widget);
    gtk_grid_set_column_homogeneous(grid, TRUE);
    int line = 0;

    d->remove_button = dt_action_button_new(
        self, N_("remove"), button_clicked, GINT_TO_POINTER(0),
        _("remove images from the image library, without deleting"), GDK_KEY_Delete, 0);
    gtk_grid_attach(grid, d->remove_button, 0, line, 2, 1);

    // delete button label and tooltip will be updated based on trash pref
    d->delete_button =
        dt_action_button_new(self, N_("delete"), button_clicked, GINT_TO_POINTER(1), NULL, 0, 0);
    gtk_grid_attach(grid, d->delete_button, 2, line++, 2, 1);

    d->move_button = dt_action_button_new(self, N_("move..."), button_clicked, GINT_TO_POINTER(8),
                                          _("move to other folder"), 0, 0);
    gtk_grid_attach(grid, d->move_button, 0, line, 2, 1);

    d->copy_button = dt_action_button_new(self, N_("copy..."), button_clicked, GINT_TO_POINTER(9),
                                          _("copy to other folder"), 0, 0);
    gtk_grid_attach(grid, d->copy_button, 2, line++, 2, 1);

    d->create_hdr_button =
        dt_action_button_new(self, N_("create HDR"), button_clicked, GINT_TO_POINTER(7),
                             _("create a high dynamic range image from selected shots"), 0, 0);
    gtk_grid_attach(grid, d->create_hdr_button, 0, line, 2, 1);

    d->duplicate_button =
        dt_action_button_new(self, N_("duplicate"), button_clicked, GINT_TO_POINTER(3),
                             _("add a duplicate to the image library, including its history stack"),
                             GDK_KEY_d, GDK_CONTROL_MASK);
    gtk_grid_attach(grid, d->duplicate_button, 2, line++, 2, 1);

    d->rotate_ccw_button = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_NONE, NULL);
    ;
    gtk_widget_set_name(d->rotate_ccw_button, "non-flat");
    gtk_widget_set_tooltip_text(d->rotate_ccw_button, _("rotate selected images 90 degrees CCW"));
    gtk_grid_attach(grid, d->rotate_ccw_button, 0, line, 1, 1);
    g_signal_connect(G_OBJECT(d->rotate_ccw_button), "clicked", G_CALLBACK(button_clicked),
                     GINT_TO_POINTER(4));
    dt_action_define(DT_ACTION(self), NULL, N_("rotate selected images 90 degrees CCW"),
                     d->rotate_ccw_button, &dt_action_def_button);

    d->rotate_cw_button = dtgtk_button_new(dtgtk_cairo_paint_refresh, 1 | CPF_NONE, NULL);
    gtk_widget_set_name(d->rotate_cw_button, "non-flat");
    gtk_widget_set_tooltip_text(d->rotate_cw_button, _("rotate selected images 90 degrees CW"));
    gtk_grid_attach(grid, d->rotate_cw_button, 1, line, 1, 1);
    g_signal_connect(G_OBJECT(d->rotate_cw_button), "clicked", G_CALLBACK(button_clicked),
                     GINT_TO_POINTER(5));
    dt_action_define(DT_ACTION(self), NULL, N_("rotate selected images 90 degrees CW"),
                     d->rotate_cw_button, &dt_action_def_button);

    d->reset_button =
        dt_action_button_new(self, N_("reset rotation"), button_clicked, GINT_TO_POINTER(6),
                             _("reset rotation to EXIF data"), 0, 0);
    gtk_grid_attach(grid, d->reset_button, 2, line++, 2, 1);

    d->cache_button = dt_action_button_new(self, N_("copy locally"), button_clicked,
                                           GINT_TO_POINTER(12), _("copy the image locally"), 0, 0);
    gtk_grid_attach(grid, d->cache_button, 0, line, 2, 1);

    d->uncache_button =
        dt_action_button_new(self, N_("resync local copy"), button_clicked, GINT_TO_POINTER(13),
                             _("synchronize the image's XMP and remove the local copy"), 0, 0);
    gtk_grid_attach(grid, d->uncache_button, 2, line++, 2, 1);

    d->group_button = dt_action_button_new(
        self, NC_("selected images action", "group"), button_clicked, GINT_TO_POINTER(10),
        _("add selected images to expanded group or create a new one"), GDK_KEY_g,
        GDK_CONTROL_MASK);
    gtk_grid_attach(grid, d->group_button, 0, line, 2, 1);

    d->ungroup_button = dt_action_button_new(
        self, N_("ungroup"), button_clicked, GINT_TO_POINTER(11),
        _("remove selected images from the group"), GDK_KEY_g, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    gtk_grid_attach(grid, d->ungroup_button, 2, line++, 2, 1);

    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_PREFERENCES_CHANGE, _image_preference_changed);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_SELECTION_CHANGED, _image_selection_changed_callback);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, _mouse_over_image_callback);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_COLLECTION_CHANGED, _collection_updated_callback);

    dt_action_register(DT_ACTION(self), N_("duplicate virgin"), _duplicate_virgin, GDK_KEY_d,
                       GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    _image_preference_changed(NULL, self); // update delete button label/tooltip
}

void gui_reset(dt_lib_module_t *self)
{
    dt_lib_gui_queue_update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
    free(self->data);
    self->data = NULL;
}
