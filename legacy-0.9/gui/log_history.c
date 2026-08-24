/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "gui/log_history.h"

#include "common/darktable.h"
#include "control/control.h"
#include "control/signal.h"
#include "gui/gtk.h"

static GtkWidget *_window = NULL;
static GtkWidget *_list_box = NULL;

static void _populate(void)
{
    if (!_list_box)
        return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(_list_box));
    for (GList *child = children; child; child = g_list_next(child))
        gtk_widget_destroy(GTK_WIDGET(child->data));
    g_list_free(children);

    GList *entries = g_list_reverse(dt_control_log_history_get_entries());
    for (GList *entry = entries; entry; entry = g_list_next(entry))
    {
        GtkWidget *label = gtk_label_new(entry->data);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        dt_gui_add_class(label, "dt_monospace");
        gtk_list_box_insert(GTK_LIST_BOX(_list_box), label, -1);
    }
    g_list_free_full(entries, g_free);

    gtk_widget_show_all(_list_box);
}

static void _log_redraw_callback(gpointer instance, gpointer user_data)
{
    _populate();
    (void)instance;
    (void)user_data;
}

static void _window_destroyed(GtkWidget *widget, gpointer user_data)
{
    if (darktable.signals)
        DT_CONTROL_SIGNAL_DISCONNECT(G_CALLBACK(_log_redraw_callback), widget);

    _list_box = NULL;
    _window = NULL;
    (void)user_data;
}

void dt_gui_log_history_show(GtkWindow *parent)
{
    if (_window)
    {
        _populate();
        gtk_window_present(GTK_WINDOW(_window));
        return;
    }

    _window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(_window), _("log history"));
    gtk_window_set_default_size(GTK_WINDOW(_window), DT_PIXEL_APPLY_DPI(700),
                                DT_PIXEL_APPLY_DPI(260));
    if (GTK_IS_WINDOW(parent))
    {
        gtk_window_set_transient_for(GTK_WINDOW(_window), parent);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(_window), TRUE);
    }

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled), GTK_SHADOW_IN);

    _list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(_list_box), GTK_SELECTION_NONE);
    gtk_widget_set_name(_list_box, "log-history-list");
    gtk_container_add(GTK_CONTAINER(scrolled), _list_box);
    gtk_container_add(GTK_CONTAINER(_window), scrolled);

    g_signal_connect(G_OBJECT(_window), "destroy", G_CALLBACK(_window_destroyed), NULL);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_LOG_REDRAW, _log_redraw_callback, _window);

    _populate();
    gtk_widget_show_all(_window);
    gtk_window_present(GTK_WINDOW(_window));
}
