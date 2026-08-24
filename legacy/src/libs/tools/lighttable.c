/*
    This file is part of darktable,
    Copyright (C) 2011-2025 darktable developers.

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

#include <gdk/gdkkeysyms.h>

#include "common/collection.h"
#include "common/debug.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef enum dt_lighttable_direct_mode_t
{
    DT_LIGHTTABLE_DIRECT_MODE_NONE = 0,
    DT_LIGHTTABLE_DIRECT_MODE_GRID,
    DT_LIGHTTABLE_DIRECT_MODE_LOUPE,
    DT_LIGHTTABLE_DIRECT_MODE_COMPARE,
    DT_LIGHTTABLE_DIRECT_MODE_SURVEY
} dt_lighttable_direct_mode_t;

typedef struct dt_lib_tool_lighttable_t
{
    GtkWidget *layout_box;
    GtkWidget *grid_smaller;
    GtkWidget *grid_larger;
    GtkWidget *loupe;
    GtkWidget *layout_culling_dynamic;
    GtkWidget *layout_culling_fix;
    GtkWidget *layout_culling_restricted;
    dt_lighttable_layout_t layout, base_layout;
    int current_zoom;
    dt_lighttable_culling_restriction_t culling_init_restriction;
    dt_lighttable_direct_mode_t direct_mode_pending;
} dt_lib_tool_lighttable_t;

/* set zoom proxy function */
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom);
static gint _lib_lighttable_get_zoom(dt_lib_module_t *self);

/* get/set layout proxy function */
static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self);

static void _set_zoom(dt_lib_module_t *self, int old_zoom, int new_zoom);

const char *name(dt_lib_module_t *self)
{
    return _("lighttable");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
    return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
    return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT;
}

gboolean expandable(dt_lib_module_t *self)
{
    return FALSE;
}

int position(const dt_lib_module_t *self)
{
    return 1001;
}

static void _lib_lighttable_update_btn(dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;

    gboolean fullpreview = dt_view_lighttable_preview_state(darktable.view_manager);

    // which btn should be active ?
    GtkWidget *active = NULL;
    if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
        active = d->layout_culling_dynamic;
    else if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
        active = d->layout_culling_fix;
    GList *children = gtk_container_get_children(GTK_CONTAINER(d->layout_box));
    for (GList *l = children; l; l = g_list_delete_link(l, l))
    {
        GtkWidget *w = l->data;
        if (GTK_IS_TOGGLE_BUTTON(w))
        {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), (w == active));
            gtk_widget_queue_draw(w); // force redraw even if state not changed
        }
    }

    if (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING || fullpreview)
        gtk_widget_set_tooltip_text(d->layout_culling_fix,
                                    _("click to enter culling layout in fixed mode."));
    else
        gtk_widget_set_tooltip_text(d->layout_culling_fix, _("click to exit culling layout."));

    if (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC || fullpreview)
        gtk_widget_set_tooltip_text(d->layout_culling_dynamic,
                                    _("click to enter culling layout in dynamic mode."));
    else
        gtk_widget_set_tooltip_text(d->layout_culling_dynamic, _("click to exit culling layout."));

    const gboolean grid_controls_active =
        d->layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && !fullpreview;
    gtk_widget_set_sensitive(d->grid_smaller,
                             grid_controls_active &&
                                 d->current_zoom > DT_LIGHTTABLE_GRID_MIN_THUMBNAIL_SIZE);
    gtk_widget_set_sensitive(d->grid_larger,
                             grid_controls_active &&
                                 d->current_zoom < DT_LIGHTTABLE_GRID_MAX_THUMBNAIL_SIZE);

    // culling restricted button configuration
    if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING || fullpreview)
    {
        if (dt_view_lighttable_culling_restricted_state(darktable.view_manager) ==
            DT_LIGHTTABLE_CULLING_RESTRICTION_SELECTION)
        {
            gtk_widget_set_tooltip_text(
                d->layout_culling_restricted,
                _("click to allow browsing all images from the collection."));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->layout_culling_restricted), TRUE);
        }
        else
        {
            gtk_widget_set_tooltip_text(d->layout_culling_restricted,
                                        _("click to limit browsing to the selection."));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->layout_culling_restricted), FALSE);
        }

        gtk_widget_set_visible(d->layout_culling_restricted, TRUE);
    }
    else
    {
        gtk_widget_set_visible(d->layout_culling_restricted, FALSE);
        // limit the filckering on next show : it's less visible to do inactive->active
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->layout_culling_restricted), FALSE);
    }
}

static void _lib_lighttable_set_layout(dt_lib_module_t *self, const dt_lighttable_layout_t layout)
{
    dt_lib_tool_lighttable_t *d = self->data;

    // we deal with fullpreview first.
    if ((layout == DT_LIGHTTABLE_LAYOUT_PREVIEW) ^
        dt_view_lighttable_preview_state(darktable.view_manager))
        dt_view_lighttable_set_preview_state(
            darktable.view_manager, layout == DT_LIGHTTABLE_LAYOUT_PREVIEW, TRUE,
            FALSE, DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO);

    if (layout == DT_LIGHTTABLE_LAYOUT_PREVIEW)
    {
        // special case for preview : we don't change previous values,
        // just show full preview and update buttons
        _lib_lighttable_update_btn(self);
        return;
    }

    const int current_layout = dt_conf_get_int("plugins/lighttable/layout");
    d->layout = layout;

    if (current_layout != layout)
    {
        if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
        {
            d->current_zoom = MAX(1, MIN(30, dt_collection_get_selected_count()));
            if (d->current_zoom == 1)
                d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
        }
        else if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
        {
            d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
        }
        else
        {
            d->current_zoom = dt_conf_get_int("plugins/lighttable/grid_thumbnail_size");
            d->current_zoom = CLAMP(d->current_zoom, DT_LIGHTTABLE_GRID_MIN_THUMBNAIL_SIZE,
                                    DT_LIGHTTABLE_GRID_MAX_THUMBNAIL_SIZE);
            dt_conf_set_int("plugins/lighttable/grid_thumbnail_size", d->current_zoom);
        }

        dt_conf_set_int("plugins/lighttable/layout", layout);
        if (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        {
            d->base_layout = layout;
            dt_conf_set_int("plugins/lighttable/base_layout", layout);
        }

        dt_control_queue_redraw_center();
    }
    else
    {
        dt_control_queue_redraw_center();
    }

    _lib_lighttable_update_btn(self);
}

static void _lib_lighttable_apply_direct_mode(dt_lib_module_t *self,
                                              const dt_lighttable_direct_mode_t mode)
{
    dt_lib_tool_lighttable_t *d = self->data;

    switch (mode)
    {
    case DT_LIGHTTABLE_DIRECT_MODE_GRID:
        _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
        break;
    case DT_LIGHTTABLE_DIRECT_MODE_LOUPE:
        _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_PREVIEW);
        break;
    case DT_LIGHTTABLE_DIRECT_MODE_COMPARE:
        d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
        _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
        _lib_lighttable_set_zoom(self, 2);
        break;
    case DT_LIGHTTABLE_DIRECT_MODE_SURVEY:
        _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
        break;
    case DT_LIGHTTABLE_DIRECT_MODE_NONE:
        break;
    }
}

static void _lib_lighttable_request_direct_mode(dt_lib_module_t *self,
                                                const dt_lighttable_direct_mode_t mode)
{
    dt_lib_tool_lighttable_t *d = self->data;
    d->direct_mode_pending = mode;

    if (dt_view_get_current() == DT_VIEW_LIGHTTABLE)
    {
        d->direct_mode_pending = DT_LIGHTTABLE_DIRECT_MODE_NONE;
        _lib_lighttable_apply_direct_mode(self, mode);
        return;
    }

    // Match the regular view shortcuts: the control layer prepares the UI and
    // performs the switch from the main loop.  The pending mode is consumed by
    // the view-changed signal below once Lighttable is ready.
    dt_ctl_switch_mode_to("lighttable");
}

static void _lib_lighttable_direct_mode_view_changed(gpointer instance, dt_view_t *old_view,
                                                     dt_view_t *new_view, dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;
    if (!d || d->direct_mode_pending == DT_LIGHTTABLE_DIRECT_MODE_NONE)
        return;

    const dt_lighttable_direct_mode_t mode = d->direct_mode_pending;
    d->direct_mode_pending = DT_LIGHTTABLE_DIRECT_MODE_NONE;
    if (new_view && new_view->view(new_view) == DT_VIEW_LIGHTTABLE)
        _lib_lighttable_apply_direct_mode(self, mode);
}

static void _lib_lighttable_direct_mode_view_cannot_change(gpointer instance, dt_view_t *old_view,
                                                           dt_view_t *new_view,
                                                           dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;
    if (d && new_view && new_view->view(new_view) == DT_VIEW_LIGHTTABLE)
        d->direct_mode_pending = DT_LIGHTTABLE_DIRECT_MODE_NONE;
}

static void _lib_lighttable_direct_grid(dt_action_t *action)
{
    _lib_lighttable_request_direct_mode(darktable.view_manager->proxy.lighttable.module,
                                        DT_LIGHTTABLE_DIRECT_MODE_GRID);
}

static void _lib_lighttable_direct_loupe(dt_action_t *action)
{
    _lib_lighttable_request_direct_mode(darktable.view_manager->proxy.lighttable.module,
                                        DT_LIGHTTABLE_DIRECT_MODE_LOUPE);
}

static void _lib_lighttable_direct_compare(dt_action_t *action)
{
    _lib_lighttable_request_direct_mode(darktable.view_manager->proxy.lighttable.module,
                                        DT_LIGHTTABLE_DIRECT_MODE_COMPARE);
}

static void _lib_lighttable_direct_survey(dt_action_t *action)
{
    _lib_lighttable_request_direct_mode(darktable.view_manager->proxy.lighttable.module,
                                        DT_LIGHTTABLE_DIRECT_MODE_SURVEY);
}

static gboolean _lib_lighttable_layout_btn_release(GtkWidget *w, GdkEventButton *event,
                                                   dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;

    const gboolean active = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(w)); // note : this is the state before the change
    dt_lighttable_layout_t new_layout = DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
    if (!active)
    {
        // that means we want to activate the button
        if (w == d->layout_culling_fix)
        {
            if (dt_modifier_is(event->state, GDK_CONTROL_MASK))
                d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION;
            else
                d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
            new_layout = DT_LIGHTTABLE_LAYOUT_CULLING;
        }
        else if (w == d->layout_culling_dynamic)
            new_layout = DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC;
    }
    else
    {
        // that means we want to deactivate the button
        if (w == d->layout_culling_dynamic || w == d->layout_culling_fix)
            new_layout = d->base_layout;
        else
        {
            // we can't exit from filemanager
            return TRUE;
        }
    }

    _lib_lighttable_set_layout(self, new_layout);
    return TRUE;
}

static gboolean _lib_lighttable_restricted_btn_release(GtkWidget *w, GdkEventButton *event,
                                                       dt_lib_module_t *self)
{
    dt_lighttable_culling_restriction_t restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_SELECTION;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)))
        restriction =
            DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION; // note : this is the state before the change

    dt_view_lighttable_set_culling_restricted_state(darktable.view_manager, restriction);
    _lib_lighttable_update_btn(self);
    return TRUE;
}

static void _lib_lighttable_grid_smaller_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;
    _lib_lighttable_set_zoom(self, d->current_zoom - DT_LIGHTTABLE_GRID_THUMBNAIL_SIZE_STEP);
    (void)widget;
}

static void _lib_lighttable_grid_larger_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;
    _lib_lighttable_set_zoom(self, d->current_zoom + DT_LIGHTTABLE_GRID_THUMBNAIL_SIZE_STEP);
    (void)widget;
}

static void _lib_lighttable_loupe_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
    _lib_lighttable_request_direct_mode(self, DT_LIGHTTABLE_DIRECT_MODE_LOUPE);
    (void)widget;
}

static void _lib_lighttable_key_accel_toggle_culling_dynamic_mode(dt_action_t *action)
{
    dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
    dt_lib_tool_lighttable_t *d = self->data;

    // if we are already in any culling layout, we return to the base layout
    if (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING &&
        d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
    }
    else
        _lib_lighttable_set_layout(self, d->base_layout);

    dt_control_queue_redraw_center();
}

static void _lib_lighttable_key_accel_toggle_culling_zoom_mode(dt_action_t *action)
{
    dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
    dt_lib_tool_lighttable_t *d = self->data;

    if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
        _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);
    else if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
        _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
    }
}

static void _lib_lighttable_key_accel_toggle_restricted_mode(dt_action_t *action)
{
    dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
    dt_lib_tool_lighttable_t *d = self->data;

    if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
        dt_view_lighttable_preview_state(darktable.view_manager))
    {
        // if we are already in culling layout or fullpreview, we switch between restricted and unrestricted
        _lib_lighttable_restricted_btn_release(d->layout_culling_restricted, NULL, self);
    }
}

static void _lib_lighttable_key_accel_exit_layout(dt_action_t *action)
{
    dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
    dt_lib_tool_lighttable_t *d = self->data;

    if (dt_view_lighttable_preview_state(darktable.view_manager))
        _lib_lighttable_set_layout(self, d->layout);
    else if (d->layout != d->base_layout)
        _lib_lighttable_set_layout(self, d->base_layout);
}

static dt_lighttable_culling_restriction_t
_lib_lighttable_get_culling_initial_restriction(dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;
    return d ? d->culling_init_restriction : DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
}

static dt_lighttable_layout_t _lib_lighttable_get_configured_layout(const char *key)
{
    const int layout = dt_conf_get_int(key);
    if (layout >= DT_LIGHTTABLE_LAYOUT_FILEMANAGER && layout < DT_LIGHTTABLE_LAYOUT_LAST)
        return layout;

    // Layout 0 was the removed zoomable lighttable. Normalise it, and any
    // invalid legacy value, before the view selects a thumbtable mode.
    dt_conf_set_int(key, DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
    return DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
}

enum
{
    DT_ACTION_ELEMENT_CULLING_NO_RESTRICTION = 1,
};

static float _action_process_culling(gpointer target, dt_action_element_t element,
                                     dt_action_effect_t effect, float move_size)
{
    dt_lib_module_t *self = darktable.view_manager->proxy.lighttable.module;
    dt_lib_tool_lighttable_t *d = self->data;

    if (DT_PERFORM_ACTION(move_size))
    {
        if (d->layout != DT_LIGHTTABLE_LAYOUT_CULLING &&
            d->layout != DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC && effect != DT_ACTION_EFFECT_ON)
        {
            // if we are not in culling layout, we enter this mode
            if (element == DT_ACTION_ELEMENT_CULLING_NO_RESTRICTION)
                d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION;
            else
                d->culling_init_restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;
            _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
        }
        else if (effect != DT_ACTION_EFFECT_ON)
        {
            // if we are already in culling layout we fallback to the base layout
            _lib_lighttable_set_layout(self, d->base_layout);
        }

        _lib_lighttable_update_btn(self);
    }

    return (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING);
}

const dt_action_element_def_t _action_elements_culling[] = {
    {N_("normal"), dt_action_effect_hold},
    {N_("no restriction"), dt_action_effect_hold},
    {NULL}};

const dt_action_def_t _action_def_culling = {N_("culling"), _action_process_culling,
                                             _action_elements_culling, NULL};

void gui_init(dt_lib_module_t *self)
{
    /* initialize ui widgets */
    dt_lib_tool_lighttable_t *d = g_malloc0(sizeof(dt_lib_tool_lighttable_t));
    self->data = (void *)d;

    dt_conf_remove_key("lighttable/zoomable/last_offset");
    dt_conf_remove_key("lighttable/zoomable/last_pos_x");
    dt_conf_remove_key("lighttable/zoomable/last_pos_y");
    dt_conf_remove_key("plugins/lighttable/images_in_row");
    d->layout = _lib_lighttable_get_configured_layout("plugins/lighttable/layout");
    d->base_layout = _lib_lighttable_get_configured_layout("plugins/lighttable/base_layout");

    if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
        d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
    else if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        d->current_zoom = MAX(1, MIN(DT_LIGHTTABLE_MAX_ZOOM, dt_collection_get_selected_count()));
        if (d->current_zoom == 1)
            d->current_zoom = dt_conf_get_int("plugins/lighttable/culling_num_images");
    }
    else
    {
        d->current_zoom = dt_conf_get_int("plugins/lighttable/grid_thumbnail_size");
        d->current_zoom = CLAMP(d->current_zoom, DT_LIGHTTABLE_GRID_MIN_THUMBNAIL_SIZE,
                                DT_LIGHTTABLE_GRID_MAX_THUMBNAIL_SIZE);
        dt_conf_set_int("plugins/lighttable/grid_thumbnail_size", d->current_zoom);
    }
    if (d->layout != DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        d->current_zoom = CLAMP(d->current_zoom, 1, DT_LIGHTTABLE_MAX_ZOOM);

    // create the layouts icon list
    dt_action_t *ltv = &darktable.view_manager->proxy.lighttable.view->actions;
    dt_action_t *ac = NULL;

    d->grid_smaller = dtgtk_button_new(dtgtk_cairo_paint_lt_grid_smaller, 0, NULL);
    ac = dt_action_define(ltv, NULL, N_("decrease grid thumbnail size"), d->grid_smaller,
                          &dt_action_def_button);
    gtk_widget_set_tooltip_text(d->grid_smaller, _("decrease grid thumbnail size"));
    g_signal_connect(G_OBJECT(d->grid_smaller), "clicked",
                     G_CALLBACK(_lib_lighttable_grid_smaller_clicked), self);

    d->grid_larger = dtgtk_button_new(dtgtk_cairo_paint_lt_grid_larger, 0, NULL);
    ac = dt_action_define(ltv, NULL, N_("increase grid thumbnail size"), d->grid_larger,
                          &dt_action_def_button);
    gtk_widget_set_tooltip_text(d->grid_larger, _("increase grid thumbnail size"));
    g_signal_connect(G_OBJECT(d->grid_larger), "clicked",
                     G_CALLBACK(_lib_lighttable_grid_larger_clicked), self);

    d->loupe = dtgtk_button_new(dtgtk_cairo_paint_lt_mode_loupe, 0, NULL);
    ac = dt_action_define(ltv, NULL, N_("loupe"), d->loupe, &dt_action_def_button);
    gtk_widget_set_tooltip_text(d->loupe, _("enter Lightroom-style loupe"));
    g_signal_connect(G_OBJECT(d->loupe), "clicked", G_CALLBACK(_lib_lighttable_loupe_clicked),
                     self);

    d->layout_culling_fix =
        dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_culling_fixed, 0, NULL);
    ac = dt_action_define(ltv, NULL, N_("toggle culling mode"), d->layout_culling_fix,
                          &_action_def_culling);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_DEFAULT, DT_ACTION_EFFECT_HOLD_TOGGLE, GDK_KEY_x, 0);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_CULLING_NO_RESTRICTION, DT_ACTION_EFFECT_HOLD_TOGGLE,
                         GDK_KEY_x, GDK_SHIFT_MASK);
    dt_gui_add_help_link(d->layout_culling_fix, "layout_culling");
    g_signal_connect(G_OBJECT(d->layout_culling_fix), "button-release-event",
                     G_CALLBACK(_lib_lighttable_layout_btn_release), self);

    d->layout_culling_dynamic =
        dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_culling_dynamic, 0, NULL);
    ac = dt_action_define(ltv, NULL, N_("toggle culling dynamic mode"), d->layout_culling_dynamic,
                          NULL);
    dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_culling_dynamic_mode, GDK_KEY_x,
                       GDK_CONTROL_MASK);
    dt_gui_add_help_link(d->layout_culling_dynamic, "layout_culling");
    g_signal_connect(G_OBJECT(d->layout_culling_dynamic), "button-release-event",
                     G_CALLBACK(_lib_lighttable_layout_btn_release), self);

    d->layout_box = dt_gui_hbox(d->grid_smaller, d->grid_larger, d->loupe,
                                d->layout_culling_fix, d->layout_culling_dynamic);
    gtk_widget_set_name(d->layout_box, "lighttable-layouts-box");

    /* culling restricted icon */
    d->layout_culling_restricted = dtgtk_togglebutton_new(dtgtk_cairo_paint_lock, 0, NULL);
    ac = dt_action_define(ltv, NULL, N_("toggle culling restricted"), d->layout_culling_restricted,
                          NULL);
    dt_action_register(ac, NULL, _lib_lighttable_key_accel_toggle_restricted_mode, GDK_KEY_r,
                       GDK_CONTROL_MASK);
    dt_gui_add_help_link(d->layout_culling_restricted, "layout_culling");
    gtk_widget_set_no_show_all(d->layout_culling_restricted, TRUE);
    g_signal_connect(G_OBJECT(d->layout_culling_restricted), "button-release-event",
                     G_CALLBACK(_lib_lighttable_restricted_btn_release), self);

    self->widget = dt_gui_hbox(d->layout_box, d->layout_culling_restricted);

    _lib_lighttable_update_btn(self);

    darktable.view_manager->proxy.lighttable.module = self;
    darktable.view_manager->proxy.lighttable.set_zoom = _lib_lighttable_set_zoom;
    darktable.view_manager->proxy.lighttable.get_zoom = _lib_lighttable_get_zoom;
    darktable.view_manager->proxy.lighttable.get_layout = _lib_lighttable_get_layout;
    darktable.view_manager->proxy.lighttable.set_layout = _lib_lighttable_set_layout;
    darktable.view_manager->proxy.lighttable.update_layout_btn = _lib_lighttable_update_btn;
    darktable.view_manager->proxy.lighttable.get_culling_initial_restriction =
        _lib_lighttable_get_culling_initial_restriction;

    dt_action_register(&darktable.control->actions_global, N_("grid"), _lib_lighttable_direct_grid,
                       GDK_KEY_g, 0);
    dt_action_register(&darktable.control->actions_global, N_("loupe"),
                       _lib_lighttable_direct_loupe, GDK_KEY_e, 0);
    dt_action_register(&darktable.control->actions_global, N_("compare"),
                       _lib_lighttable_direct_compare, GDK_KEY_c, 0);
    dt_action_register(&darktable.control->actions_global, N_("survey"),
                       _lib_lighttable_direct_survey, GDK_KEY_n, 0);

    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                             _lib_lighttable_direct_mode_view_changed);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_VIEWMANAGER_VIEW_CANNOT_CHANGE,
                             _lib_lighttable_direct_mode_view_cannot_change);

    dt_action_register(ltv, N_("toggle culling zoom mode"),
                       _lib_lighttable_key_accel_toggle_culling_zoom_mode, GDK_KEY_less, 0);
    dt_action_register(ltv, N_("exit current layout"), _lib_lighttable_key_accel_exit_layout,
                       GDK_KEY_Escape, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
    g_free(self->data);
    self->data = NULL;
}

static void _set_zoom(dt_lib_module_t *self, const int old_zoom, const int new_zoom)
{
    dt_lib_tool_lighttable_t *d = self->data;
    if (d->layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
        dt_conf_set_int("plugins/lighttable/culling_num_images", new_zoom);
        dt_control_queue_redraw_center();
    }
    else if (d->layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
        dt_conf_set_int("plugins/lighttable/grid_thumbnail_size", new_zoom);
        dt_thumbtable_zoom_changed(dt_ui_thumbtable(darktable.gui->ui), old_zoom, new_zoom);
        dt_control_queue_redraw_center();
    }
}

static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;
    return d ? d->layout : DT_LIGHTTABLE_LAYOUT_FILEMANAGER;
}

static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom)
{
    dt_lib_tool_lighttable_t *d = self->data;
    if (d->layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        zoom = CLAMP(zoom, DT_LIGHTTABLE_GRID_MIN_THUMBNAIL_SIZE,
                     DT_LIGHTTABLE_GRID_MAX_THUMBNAIL_SIZE);
    else
        zoom = CLAMP(zoom, 1, DT_LIGHTTABLE_MAX_ZOOM);
    if (zoom == d->current_zoom)
        return;

    const int old_zoom = d->current_zoom;
    d->current_zoom = zoom;
    _set_zoom(self, old_zoom, zoom);
    _lib_lighttable_update_btn(self);
}

static gint _lib_lighttable_get_zoom(dt_lib_module_t *self)
{
    dt_lib_tool_lighttable_t *d = self->data;
    return d->current_zoom;
}
