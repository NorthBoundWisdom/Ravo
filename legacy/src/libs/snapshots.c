/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

#include "common/darktable.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/history_snapshot.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/context_menu.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define HANDLE_SIZE 0.02
#define MAX_SNAPSHOT 10

// the snapshot offset in the memory table to use an area not used by the
// undo/redo support.
#define SNAPSHOT_ID_OFFSET 0xFFFFFF00

/* a snapshot */
typedef struct dt_lib_snapshot_t
{
    GtkWidget *button;
    GtkWidget *num;
    GtkWidget *status;
    GtkWidget *name;
    GtkWidget *entry;
    GtkWidget *restore_button;
    GtkWidget *bbox;
    char *module;
    char *label;
    dt_view_context_t ctx;
    dt_imgid_t imgid;
    uint32_t history_end;
    uint32_t id;
    uint8_t *buf;
    float scale;
    size_t width, height;
    dt_dev_zoom_pos_t zoom_pos;
} dt_lib_snapshot_t;

typedef struct dt_lib_snapshots_t
{
    GtkWidget *snapshots_box;

    int selected;
    gboolean snap_requested;
    guint expose_again_timeout_id;

    /* current active snapshots */
    uint32_t num_snapshots;

    /* snapshots */
    dt_lib_snapshot_t snapshot[MAX_SNAPSHOT];

    /* change snapshot overlay controls */
    gboolean dragging, vertical, inverted, panning, sidebyside;
    double vp_width, vp_height, vp_xpointer, vp_ypointer, vp_xrotate, vp_yrotate;
    gboolean on_going;
    gboolean rotsym_lightup;

    GtkWidget *take_button, *sidebyside_button;
    dt_action_t *context_show_action, *context_restore_action, *context_rename_action;
} dt_lib_snapshots_t;

typedef struct dt_snapshot_row_context_t
{
    dt_lib_module_t *self;
    GWeakRef button;
} dt_snapshot_row_context_t;

/* callback for take snapshot */
static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, dt_lib_module_t *self);

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, dt_lib_module_t *self);

static void _lib_snapshots_restore_callback(GtkButton *widget, dt_lib_module_t *self);

static int _lib_snapshots_get_activated(dt_lib_module_t *self, GtkWidget *widget);

static void _snapshot_row_context_destroy(gpointer data)
{
    dt_snapshot_row_context_t *context = data;
    if (!context)
        return;

    g_weak_ref_clear(&context->button);
    g_free(context);
}

static dt_snapshot_row_context_t *_snapshot_row_context_new(dt_lib_module_t *self,
                                                             GtkWidget *button)
{
    if (!self || !button)
        return NULL;

    dt_snapshot_row_context_t *context = g_malloc0(sizeof(*context));
    context->self = self;
    g_weak_ref_init(&context->button, G_OBJECT(button));
    return context;
}

static int _snapshot_row_context_index(const dt_action_t *action, GtkWidget **button)
{
    dt_snapshot_row_context_t *context = dt_gui_context_menu_get_action_payload(action);
    if (!context || !context->self || !context->self->data)
        return -1;

    GtkWidget *snapshot_button = g_weak_ref_get(&context->button);
    if (!snapshot_button)
        return -1;

    const int index = _lib_snapshots_get_activated(context->self, snapshot_button);
    if (index < 0)
    {
        g_object_unref(snapshot_button);
        return -1;
    }

    if (button)
        *button = snapshot_button;
    else
        g_object_unref(snapshot_button);
    return index;
}

static void _snapshot_show_context_action(dt_action_t *action)
{
    GtkWidget *button = NULL;
    if (_snapshot_row_context_index(action, &button) >= 0)
    {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
        g_object_unref(button);
    }
}

static void _snapshot_restore_context_action(dt_action_t *action)
{
    GtkWidget *button = NULL;
    const int index = _snapshot_row_context_index(action, &button);
    dt_snapshot_row_context_t *context = dt_gui_context_menu_get_action_payload(action);
    if (index >= 0 && context)
    {
        dt_lib_snapshots_t *snapshots = context->self->data;
        _lib_snapshots_restore_callback(GTK_BUTTON(snapshots->snapshot[index].restore_button),
                                        context->self);
    }
    if (button)
        g_object_unref(button);
}

static void _snapshot_rename_context_action(dt_action_t *action)
{
    GtkWidget *button = NULL;
    const int index = _snapshot_row_context_index(action, &button);
    dt_snapshot_row_context_t *context = dt_gui_context_menu_get_action_payload(action);
    if (index >= 0 && context)
    {
        dt_lib_snapshots_t *snapshots = context->self->data;
        gtk_widget_hide(snapshots->snapshot[index].name);
        gtk_widget_show(snapshots->snapshot[index].entry);
        gtk_widget_grab_focus(snapshots->snapshot[index].entry);
    }
    if (button)
        g_object_unref(button);
}

static GtkWidget *_snapshot_context_item(const gchar *label, dt_action_t *action,
                                         dt_lib_module_t *self, GtkWidget *button)
{
    dt_snapshot_row_context_t *context = _snapshot_row_context_new(self, button);
    if (!context)
        return NULL;

    return dt_gui_context_menu_action_item_new(label, action, 0, DT_ACTION_ELEMENT_DEFAULT,
                                                DT_ACTION_EFFECT_DEFAULT_KEY, context,
                                                _snapshot_row_context_destroy);
}

static void _snapshot_show_context_menu(dt_lib_module_t *self, GtkWidget *button,
                                        GdkEventButton *event)
{
    dt_lib_snapshots_t *d = self->data;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = _snapshot_context_item(_("show snapshot"), d->context_show_action, self,
                                             button);
    if (!item)
    {
        gtk_widget_destroy(menu);
        return;
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = _snapshot_context_item(_("restore snapshot"), d->context_restore_action, self, button);
    if (item)
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    item = _snapshot_context_item(_("rename snapshot"), d->context_rename_action, self, button);
    if (item)
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all(menu);
    if (event)
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    else
        dt_gui_menu_popup(GTK_MENU(menu), button, GDK_GRAVITY_SOUTH_WEST,
                          GDK_GRAVITY_NORTH_WEST);
}

static gboolean _snapshot_button_press_context(GtkWidget *widget, GdkEventButton *event,
                                               dt_lib_module_t *self)
{
    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY ||
        dt_modifier_is(event->state, GDK_CONTROL_MASK))
        return FALSE;

    _snapshot_show_context_menu(self, widget, event);
    return TRUE;
}

static gboolean _snapshot_popup_menu(GtkWidget *widget, dt_lib_module_t *self)
{
    _snapshot_show_context_menu(self, widget, NULL);
    return TRUE;
}

const char *name(dt_lib_module_t *self)
{
    return _("snapshots");
}

const char *description(dt_lib_module_t *self)
{
    return _("remember a specific edit state and\n"
             "allow comparing it against another\n"
             "or returning to that version");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
    return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
    return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
    return 1000;
}

enum _lib_snapshot_button_items
{
    _SNAPSHOT_BUTTON_NUM,
    _SNAPSHOT_BUTTON_STATUS,
    _SNAPSHOT_BUTTON_NAME,
    _SNAPSHOT_BUTTON_ENTRY,
} _lib_snapshot_button_items;

static GtkWidget *_lib_snapshot_button_get_item(GtkWidget *button, const int num)
{
    GtkWidget *cont = gtk_bin_get_child(GTK_BIN(button));
    GList *items = gtk_container_get_children(GTK_CONTAINER(cont));
    return (GtkWidget *)g_list_nth_data(items, num);
}

// draw snapshot sign
static void _draw_sym(cairo_t *cr, const float x, const float y, const gboolean vertical,
                      const gboolean inverted)
{
    const double inv = inverted ? -0.1 : 1.0;

    PangoRectangle ink;
    PangoFontDescription *desc =
        pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(12) * PANGO_SCALE);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, C_("snapshot sign", "S"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);

    if (vertical)
        cairo_move_to(cr, x - (inv * ink.width * 1.2f),
                      y - (ink.height / 2.0f) - DT_PIXEL_APPLY_DPI(3));
    else
        cairo_move_to(cr, x - (ink.width / 2.0),
                      y + (-inv * (ink.height * 1.2f) - DT_PIXEL_APPLY_DPI(2)));

    dt_draw_set_color_overlay(cr, FALSE, 0.9);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
}

static gboolean _snap_expose_again(gpointer user_data)
{
    dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)user_data;

    d->expose_again_timeout_id = 0;
    d->snap_requested = TRUE;
    dt_control_queue_redraw_center();
    return FALSE;
}

/* check if (x,y) closer to rotation sym than area_size. Set the size of area s
   and the center of the sym (rx, ry). Return TRUE if (x,y) in sym area. */
static inline gboolean _get_rotation_area(dt_lib_module_t *self, const int32_t x, const int32_t y,
                                          double *s, gint *rx, gint *ry)
{
    dt_lib_snapshots_t *d = self->data;

    const double _s = fmin(24, d->vp_width * HANDLE_SIZE);
    const gint _rx = (d->vertical ? d->vp_width * d->vp_xpointer : d->vp_width * 0.5) - (_s * 0.5);
    const gint _ry =
        (d->vertical ? d->vp_height * 0.5 : d->vp_height * d->vp_ypointer) - (_s * 0.5);

    if (s)
        *s = _s;
    if (rx)
        *rx = _rx;
    if (ry)
        *ry = _ry;

    const int area_size = 40;

    // rotation symbol is light-up or light-off when moving close.
    return (abs(x - _rx) < area_size) && (abs(y - _ry) < area_size);
}

/* expose snapshot over center viewport */
void gui_post_expose(dt_lib_module_t *self, cairo_t *cri, const int32_t width, const int32_t height,
                     const int32_t pointerx, const int32_t pointery)
{
    dt_lib_snapshots_t *d = self->data;
    dt_develop_t *dev = darktable.develop;

    if (d->sidebyside && (!darktable.gui->drawing_snapshot ^ !d->inverted))
        return;

    if (d->selected >= 0)
    {
        dt_lib_snapshot_t *snap = &d->snapshot[d->selected];

        const dt_view_context_t ctx = dt_view_get_context_hash();

        // if a new snapshot is needed, do this now
        if (d->snap_requested && snap->ctx == ctx)
        {
            dt_free_align(snap->buf);
            snap->buf = NULL;

            // export image with proper size
            dt_dev_image(snap->imgid, width, height, snap->history_end, &snap->buf, &snap->scale,
                         &snap->width, &snap->height, snap->zoom_pos, snap->id, NULL,
                         DT_DEVICE_NONE, FALSE);
            d->snap_requested = FALSE;
            d->expose_again_timeout_id = 0;
        }

        // if ctx has changed, get a new snapshot at the right zoom
        // level. this is using a time out to ensure we don't try to
        // create many snapshot while zooming (this is slow), so we wait
        // to the zoom level to be stabilized to create the new snapshot.
        if (snap->ctx != ctx || !snap->buf)
        {
            // request a new snapshot in the following conditions:
            //    1. we are not panning
            //    2. the mouse is not over the center area, probably panning
            //    with the navigation module

            snap->ctx = ctx;
            if (!d->panning && dev->darkroom_mouse_in_center_area)
                d->snap_requested = TRUE;
            if (d->expose_again_timeout_id != 0)
                g_source_remove(d->expose_again_timeout_id);

            d->expose_again_timeout_id = g_timeout_add(150, _snap_expose_again, d);
        }

        float pzx, pzy, zoom_scale;
        dt_dev_get_pointer_zoom_pos(&dev->full, 0, 0, &pzx, &pzy, &zoom_scale);

        pzx = fmin(pzx + 0.5f, 0.0f);
        pzy = fmin(pzy + 0.5f, 0.0f);

        d->vp_width = width;
        d->vp_height = height;

        const double lx = d->sidebyside ? d->inverted ? 0 : width : width * d->vp_xpointer;
        const double ly = d->sidebyside ? d->inverted ? 0 : height : height * d->vp_ypointer;

        const double size = DT_PIXEL_APPLY_DPI(d->inverted ? -15 : 15);

        // clear background
        dt_gui_gtk_set_source_rgb(cri, DT_GUI_COLOR_DARKROOM_BG);
        if (d->vertical)
        {
            if (d->inverted)
                cairo_rectangle(cri, lx, 0, width - lx, height);
            else
                cairo_rectangle(cri, 0, 0, lx, height);
        }
        else
        {
            if (d->inverted)
                cairo_rectangle(cri, 0, ly, width, height - ly);
            else
                cairo_rectangle(cri, 0, 0, width, ly);
        }
        cairo_save(cri);
        cairo_clip(cri);
        cairo_fill(cri);

        if (snap->buf)
        {
            dt_view_paint_surface(cri, width, height, &dev->full, DT_WINDOW_MAIN, snap->buf,
                                  snap->scale, snap->width, snap->height, snap->zoom_pos);
        }

        cairo_restore(cri);

        // draw the split line using the selected overlay color
        dt_draw_set_color_overlay(cri, TRUE, 0.7);

        cairo_set_line_width(cri, 1.);

        if (d->vertical)
        {
            const float iheight = dev->preview_pipe->backbuf_height * zoom_scale;
            const double offset = (double)(iheight * (-pzy));
            const double center = (fabs(size) * 2.0) + offset;

            // line
            cairo_move_to(cri, lx, 0.0f);
            cairo_line_to(cri, lx, height);
            cairo_stroke(cri);

            if (!d->dragging)
            {
                // triangle
                cairo_move_to(cri, lx, center - size);
                cairo_line_to(cri, lx - (size * 1.2), center);
                cairo_line_to(cri, lx, center + size);
                cairo_close_path(cri);
                cairo_fill(cri);

                // symbol
                _draw_sym(cri, lx, center, TRUE, d->inverted);
            }
        }
        else
        {
            const float iwidth = dev->preview_pipe->backbuf_width * zoom_scale;
            const double offset = (double)(iwidth * (-pzx));
            const double center = (fabs(size) * 2.0) + offset;

            // line
            cairo_move_to(cri, 0.0f, ly);
            cairo_line_to(cri, width, ly);
            cairo_stroke(cri);

            if (!d->dragging)
            {
                // triangle
                cairo_move_to(cri, center - size, ly);
                cairo_line_to(cri, center, ly - (size * 1.2));
                cairo_line_to(cri, center + size, ly);
                cairo_close_path(cri);
                cairo_fill(cri);

                // symbol
                _draw_sym(cri, center, ly, FALSE, d->inverted);
            }
        }

        /* if mouse over control lets draw center rotate control, hide if split is dragged */
        if (!d->dragging && !d->sidebyside)
        {
            double s = 0.0;
            gint rx = 0;
            gint ry = 0;

            d->rotsym_lightup = _get_rotation_area(self, pointerx, pointery, &s, &rx, &ry);

            dt_draw_set_color_overlay(cri, TRUE, d->rotsym_lightup ? 1.0 : 0.3);

            cairo_set_line_width(cri, 0.5);
            dtgtk_cairo_paint_refresh(cri, rx, ry, s, s, 0, NULL);
        }

        d->on_going = FALSE;
    }
}

int button_released(struct dt_lib_module_t *self, const double x, const double y, const int which,
                    const uint32_t state)
{
    dt_lib_snapshots_t *d = self->data;

    if (d->panning)
    {
        d->panning = FALSE;
        return 0;
    }

    if (d->selected >= 0)
    {
        d->dragging = FALSE;
        return 1;
    }
    return 0;
}

static int _lib_snapshot_rotation_cnt = 0;

int button_pressed(struct dt_lib_module_t *self, const double x, const double y,
                   const double pressure, const int which, const int type, const uint32_t state)
{
    dt_lib_snapshots_t *d = self->data;

    if (darktable.develop->darkroom_skip_mouse_events)
    {
        d->panning = TRUE;
        return 0;
    }

    if (d->selected >= 0 && which != GDK_BUTTON_MIDDLE)
    {
        if (d->on_going)
            return 1;

        const double xp = x / d->vp_width;
        const double yp = y / d->vp_height;

        /* do the split rotating */
        const double hhs = HANDLE_SIZE * 0.5;
        if (((d->vertical && xp > d->vp_xpointer - hhs && xp < d->vp_xpointer + hhs) &&
             yp > 0.5 - hhs && yp < 0.5 + hhs) ||
            ((!d->vertical && yp > d->vp_ypointer - hhs && yp < d->vp_ypointer + hhs) &&
             xp > 0.5 - hhs && xp < 0.5 + hhs) ||
            d->sidebyside ||
            (d->vp_xrotate > xp - hhs && d->vp_xrotate <= xp + hhs && d->vp_yrotate > yp - hhs &&
             d->vp_yrotate <= yp + hhs))
        {
            /* let's rotate */
            _lib_snapshot_rotation_cnt++;

            d->vertical = !d->vertical;
            gtk_orientable_set_orientation(
                GTK_ORIENTABLE(gtk_widget_get_parent(dt_ui_snapshot(darktable.gui->ui))),
                d->vertical ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
            if (_lib_snapshot_rotation_cnt % 2)
                d->inverted = !d->inverted;
            if (d->sidebyside)
                d->snap_requested = TRUE;

            d->vp_xpointer = xp;
            d->vp_ypointer = yp;
            d->vp_xrotate = xp;
            d->vp_yrotate = yp;
            d->on_going = TRUE;
            dt_control_queue_redraw_center();
        }
        /* do the dragging !? */
        else
        {
            d->dragging = TRUE;
            d->vp_ypointer = yp;
            d->vp_xpointer = xp;
            d->vp_xrotate = 0.0;
            d->vp_yrotate = 0.0;
            dt_control_queue_redraw_center();
        }
        return 1;
    }
    return 0;
}

int mouse_moved(dt_lib_module_t *self, const double x, const double y, const double pressure,
                const int which)
{
    dt_lib_snapshots_t *d = self->data;

    // if panning, do not handle here, let darkroom do the job
    if (d->panning)
        return 0;

    if (d->selected >= 0)
    {
        const double xp = x / d->vp_width;
        const double yp = y / d->vp_height;

        /* update x pointer */
        if (d->dragging)
        {
            d->vp_xpointer = xp;
            d->vp_ypointer = yp;
        }

        // Here to ensure the rotation symbol is light-up or light-off
        // when moving close.
        const gboolean display_rotation = _get_rotation_area(self, x, y, NULL, NULL, NULL);

        if (d->dragging || display_rotation != d->rotsym_lightup)
            dt_control_queue_redraw_center();

        return 1;
    }

    return 0;
}

static void _lib_snapshots_toggle_last(dt_action_t *action)
{
    dt_lib_snapshots_t *d = dt_action_lib(action)->data;

    const int32_t index = d->num_snapshots - 1;

    if (d->num_snapshots)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(d->snapshot[index].button),
            !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->snapshot[index].button)));
}

static int _look_for_widget(dt_lib_module_t *self, GtkWidget *widget, gboolean entry)
{
    dt_lib_snapshots_t *d = self->data;

    for (int k = 0; k < MAX_SNAPSHOT; k++)
    {
        if ((entry ? d->snapshot[k].entry : d->snapshot[k].button) == widget)
            return k;
    }

    return 0;
}

static void _entry_activated_callback(GtkEntry *entry, dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    const int index = _look_for_widget(self, (GtkWidget *)entry, TRUE);

    const char *txt = gtk_entry_get_text(GTK_ENTRY(d->snapshot[index].entry));

    char *label = dt_history_get_name_label(d->snapshot[index].module, txt, TRUE, TRUE);
    gtk_label_set_markup(GTK_LABEL(d->snapshot[index].name), label);
    g_free(label);

    gtk_widget_hide(d->snapshot[index].entry);
    gtk_widget_show(d->snapshot[index].name);
    gtk_widget_grab_focus(d->snapshot[index].button);
}

static gboolean _lib_button_button_pressed_callback(GtkWidget *widget, GdkEventButton *event,
                                                    dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    const int index = _look_for_widget(self, widget, FALSE);

    if (dt_modifier_is(event->state, GDK_CONTROL_MASK))
    {
        gtk_widget_hide(d->snapshot[index].name);
        gtk_widget_show(d->snapshot[index].entry);
        gtk_widget_grab_focus(d->snapshot[index].entry);
    }

    gtk_widget_set_focus_on_click(widget, FALSE);
    return gtk_widget_has_focus(d->snapshot[index].entry);
}

static void _init_snapshot_entry(dt_lib_module_t *self, dt_lib_snapshot_t *s)
{
    /* create snapshot button */
    s->button = gtk_toggle_button_new();
    gtk_widget_set_name(s->button, "snapshot-button");
    g_signal_connect(G_OBJECT(s->button), "toggled", G_CALLBACK(_lib_snapshots_toggled_callback),
                     self);
    g_signal_connect(G_OBJECT(s->button), "button-press-event",
                     G_CALLBACK(_lib_button_button_pressed_callback), self);
    g_signal_connect(G_OBJECT(s->button), "button-press-event",
                     G_CALLBACK(_snapshot_button_press_context), self);
    g_signal_connect(G_OBJECT(s->button), "popup-menu", G_CALLBACK(_snapshot_popup_menu), self);

    s->num = gtk_label_new("");
    gtk_widget_set_name(s->num, "history-number");
    dt_gui_add_class(s->num, "dt_monospace");

    s->status = gtk_label_new("");
    dt_gui_add_class(s->status, "dt_monospace");

    s->name = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(s->name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(s->name, GTK_ALIGN_START);

    s->entry = gtk_entry_new();
    gtk_widget_set_halign(s->entry, GTK_ALIGN_FILL);
    g_signal_connect(G_OBJECT(s->entry), "activate", G_CALLBACK(_entry_activated_callback), self);

    s->restore_button = dtgtk_button_new(dtgtk_cairo_paint_snapshots_restore, CPF_NONE, NULL);
    gtk_widget_set_name(s->restore_button, "non-flat");
    gtk_widget_set_tooltip_text(s->restore_button, _("restore snapshot into current history"));
    g_signal_connect(G_OBJECT(s->restore_button), "clicked",
                     G_CALLBACK(_lib_snapshots_restore_callback), self);
}

static void _clear_snapshot_entry(dt_lib_snapshot_t *s)
{
    // delete corresponding entry from the database

    dt_history_snapshot_clear(s->imgid, s->id);

    s->ctx = 0;
    s->imgid = NO_IMGID;
    s->history_end = -1;

    if (s->button)
    {
        GtkWidget *lstatus = _lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_STATUS);
        gtk_widget_set_tooltip_text(s->button, "");
        gtk_widget_set_tooltip_text(lstatus, "");
        gtk_widget_hide(s->button);
        gtk_widget_hide(s->restore_button);
    }

    g_free(s->module);
    g_free(s->label);
    dt_free_align(s->buf);
    s->module = NULL;
    s->label = NULL;
    s->buf = NULL;
}

static void _clear_snapshots(dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;
    d->selected = -1;
    darktable.lib->proxy.snapshots.enabled = FALSE;
    d->snap_requested = FALSE;

    for (uint32_t k = 0; k < d->num_snapshots; k++)
    {
        dt_lib_snapshot_t *s = &d->snapshot[k];
        s->id = SNAPSHOT_ID_OFFSET | k;
        _clear_snapshot_entry(s);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->button), FALSE);
    }

    d->num_snapshots = 0;
    gtk_widget_set_sensitive(d->take_button, TRUE);

    dt_control_queue_redraw_center();
}

void gui_reset(dt_lib_module_t *self)
{
    _clear_snapshots(self);
}

static void _signal_profile_changed(gpointer instance, const uint8_t profile_type,
                                    dt_lib_module_t *self)
{
    // when the display profile is changed, make sure we recreate the snapshot
    if (profile_type == DT_COLORSPACES_PROFILE_TYPE_DISPLAY)
    {
        dt_lib_snapshots_t *d = self->data;

        if (d->selected >= 0)
            d->snap_requested = TRUE;

        dt_control_queue_redraw_center();
    }
}

static void _remove_snapshot_entry(dt_lib_module_t *self, const uint32_t index)
{
    dt_lib_snapshots_t *d = self->data;

    //  First clean the entry
    _clear_snapshot_entry(&d->snapshot[index]);

    //  Repack all entries
    for (uint32_t k = index; k < MAX_SNAPSHOT - 1; k++)
    {
        memcpy(&d->snapshot[k], &d->snapshot[k + 1], sizeof(dt_lib_snapshot_t));
    }

    //  And finally clear last entry
    _clear_snapshot_entry(&d->snapshot[MAX_SNAPSHOT - 1]);
    //  And dedup widgets by initializing the last entry
    _init_snapshot_entry(self, &d->snapshot[MAX_SNAPSHOT - 1]);

    //  We have one less snapshot
    d->num_snapshots--;

    //  If the remove image snapshot was selected, unselect it
    if (d->selected == index)
        d->selected = -1;
}

static void _signal_image_removed(gpointer instance, const dt_imgid_t imgid, dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    uint32_t k = 0;

    while (k < MAX_SNAPSHOT)
    {
        dt_lib_snapshot_t *s = &d->snapshot[k];

        if (s->imgid == imgid)
        {
            _remove_snapshot_entry(self, k);
            dt_control_log(_("snapshots for removed image have been deleted"));
        }
        else
            k++;
    }
}

static void _signal_image_changed(gpointer instance, dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    const dt_imgid_t imgid = darktable.develop->image_storage.id;

    for (uint32_t k = 0; k < MAX_SNAPSHOT; k++)
    {
        dt_lib_snapshot_t *s = &d->snapshot[k];

        if (!dt_is_valid_imgid(s->imgid))
            continue;

        GtkWidget *b = d->snapshot[k].button;
        GtkWidget *st = _lib_snapshot_button_get_item(b, _SNAPSHOT_BUTTON_STATUS);

        char stat[8] = {0};

        if (s->imgid == imgid)
        {
            g_strlcpy(stat, " ", sizeof(stat));

            gtk_widget_set_tooltip_text(b, "");
            gtk_widget_set_tooltip_text(st, "");
        }
        else
        {
            g_strlcpy(stat, "↗", sizeof(stat));

            char tooltip[128] = {0};
            // tooltip
            char *name = dt_image_get_filename(s->imgid);
            snprintf(tooltip, sizeof(tooltip), _("↗ %s '%s'"), _("this snapshot was taken from"),
                     name);
            g_free(name);
            gtk_widget_set_tooltip_text(b, tooltip);
            gtk_widget_set_tooltip_text(st, tooltip);
        }

        gtk_label_set_text(GTK_LABEL(st), stat);
    }

    dt_control_queue_redraw_center();
}

static void _sidebyside_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    d->sidebyside = !d->sidebyside;
    d->snap_requested = TRUE;
    gtk_widget_set_visible(dt_ui_snapshot(darktable.gui->ui), d->sidebyside && d->selected >= 0);
}

void gui_init(dt_lib_module_t *self)
{
    /* initialize ui widgets */
    dt_lib_snapshots_t *d = g_malloc0(sizeof(dt_lib_snapshots_t));
    self->data = (void *)d;

    /* initialize snapshot storages */
    d->vp_xpointer = 0.5;
    d->vp_ypointer = 0.5;
    d->vp_xrotate = 0.0;
    d->vp_yrotate = 0.0;
    d->vertical = TRUE;
    d->on_going = FALSE;
    d->rotsym_lightup = FALSE;
    d->panning = FALSE;
    d->selected = -1;
    d->snap_requested = FALSE;
    d->expose_again_timeout_id = 0;
    d->num_snapshots = 0;
    darktable.lib->proxy.snapshots.enabled = FALSE;

    d->context_show_action = dt_action_register(DT_ACTION(self), N_("show snapshot"),
                                                _snapshot_show_context_action, 0, 0);
    d->context_restore_action = dt_action_register(DT_ACTION(self), N_("restore snapshot"),
                                                   _snapshot_restore_context_action, 0, 0);
    d->context_rename_action = dt_action_register(DT_ACTION(self), N_("rename snapshot"),
                                                  _snapshot_rename_context_action, 0, 0);
    dt_action_set_context_menu_provider_only(d->context_show_action, TRUE);
    dt_action_set_context_menu_provider_only(d->context_restore_action, TRUE);
    dt_action_set_context_menu_provider_only(d->context_rename_action, TRUE);

    /* initialize ui containers */
    self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    d->snapshots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* create take snapshot button */
    d->take_button = dt_action_button_new(self, N_("take snapshot"),
                                          _lib_snapshots_add_button_clicked_callback, self,
                                          _("take snapshot to compare with another image "
                                            "or the same image at another stage of development"),
                                          0, 0);

    /*
   * initialize snapshots
   */
    char localtmpdir[PATH_MAX] = {0};
    dt_loc_get_tmp_dir(localtmpdir, sizeof(localtmpdir));

    for (int k = 0; k < MAX_SNAPSHOT; k++)
    {
        dt_lib_snapshot_t *s = &d->snapshot[k];
        s->id = SNAPSHOT_ID_OFFSET | k;

        _clear_snapshot_entry(s);
        _init_snapshot_entry(self, s);

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        // 4 items inside box, num, status, name, label

        gtk_box_pack_start(GTK_BOX(box), s->num, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), s->status, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), s->name, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), s->entry, TRUE, TRUE, 0);

        gtk_widget_show_all(box);

        // hide entry, will be used only when editing
        gtk_widget_hide(s->entry);

        gtk_container_add(GTK_CONTAINER(s->button), box);

        // add snap button and restore button
        s->bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(s->bbox), s->button, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(s->bbox), s->restore_button, FALSE, FALSE, 0);

        /* add button to snapshot box */
        gtk_box_pack_end(GTK_BOX(d->snapshots_box), s->bbox, FALSE, FALSE, 0);

        /* prevent widget to show on external show all */
        gtk_widget_set_no_show_all(s->button, TRUE);
        gtk_widget_set_no_show_all(s->restore_button, TRUE);
    }

    /* add snapshot box and take snapshot button to widget ui*/
    gtk_box_pack_start(
        GTK_BOX(self->widget),
        dt_ui_resize_wrap(d->snapshots_box, 1, "plugins/darkroom/snapshots/windowheight"), TRUE,
        TRUE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hbox), d->take_button, TRUE, TRUE, 0);
    d->sidebyside_button =
        dtgtk_togglebutton_new(dtgtk_cairo_paint_lt_mode_culling_dynamic, 0, NULL);
    dt_action_define(DT_ACTION(self), NULL, N_("side-by-side"), d->sidebyside_button,
                     &dt_action_def_toggle);
    gtk_box_pack_start(GTK_BOX(hbox), d->sidebyside_button, FALSE, TRUE, 0);
    g_signal_connect(G_OBJECT(d->sidebyside_button), "clicked",
                     G_CALLBACK(_sidebyside_button_clicked), self);
    gtk_widget_set_tooltip_text(
        GTK_WIDGET(d->sidebyside_button),
        _("place the snapshot side-by-side / above-below the current image instead of overlaying"));

    gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

    dt_action_register(DT_ACTION(self), N_("toggle last snapshot"), _lib_snapshots_toggle_last, 0,
                       0);

    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, _signal_profile_changed);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _signal_image_changed);
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_IMAGE_REMOVED, _signal_image_removed);
}

void gui_cleanup(dt_lib_module_t *self)
{
    _clear_snapshots(self);

    g_free(self->data);
    self->data = NULL;
}

static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    // first make sure the current history is properly written
    dt_dev_write_history(darktable.develop);

    dt_lib_snapshot_t *s = &d->snapshot[d->num_snapshots];

    // set new snapshot_id, to not clash with the undo snapshot make the snapshot
    // id at a specific offset.
    s->id = SNAPSHOT_ID_OFFSET | d->num_snapshots;

    _clear_snapshot_entry(s);

    if (darktable.develop->history_end > 0)
    {
        dt_dev_history_item_t *history_item =
            g_list_nth_data(darktable.develop->history, darktable.develop->history_end - 1);
        if (history_item && history_item->module)
        {
            s->module = g_strdup(history_item->module->name());

            if (strlen(history_item->multi_name) > 0 && history_item->multi_name[0] != ' ')
            {
                s->label = history_item->multi_name_hand_edited ?
                               g_strdup(history_item->multi_name) :
                               dt_util_localize_segmented_name(history_item->multi_name, TRUE);
            }
        }
        else
            s->module = g_strdup(_("unknown"));
    }
    else
        s->module = g_strdup(_("original"));

    s->history_end = darktable.develop->history_end;
    s->imgid = darktable.develop->image_storage.id;

    dt_history_snapshot_create(s->imgid, s->id, s->history_end);

    GtkLabel *lnum = (GtkLabel *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_NUM);
    GtkLabel *lstatus =
        (GtkLabel *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_STATUS);
    GtkLabel *lname = (GtkLabel *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_NAME);
    GtkEntry *lentry = (GtkEntry *)_lib_snapshot_button_get_item(s->button, _SNAPSHOT_BUTTON_ENTRY);

    char num[8];
    g_snprintf(num, sizeof(num), "%2u", s->history_end);

    gtk_label_set_text(lnum, num);
    gtk_label_set_text(lstatus, " ");

    char *txt = dt_history_get_name_label(s->module, s->label, TRUE, TRUE);
    gtk_label_set_markup(lname, txt);

    gtk_entry_set_text(lentry, s->label ? s->label : "");

    gtk_widget_grab_focus(s->button);

    g_free(txt);

    /* update slots used */
    d->num_snapshots++;

    /* show active snapshot slots */
    for (uint32_t k = 0; k < d->num_snapshots; k++)
    {
        gtk_widget_show(d->snapshot[k].button);
        gtk_widget_show(d->snapshot[k].restore_button);
    }

    if (d->num_snapshots == MAX_SNAPSHOT)
        gtk_widget_set_sensitive(d->take_button, FALSE);
}

static int _lib_snapshots_get_activated(dt_lib_module_t *self, GtkWidget *widget)
{
    dt_lib_snapshots_t *d = self->data;

    for (uint32_t k = 0; k < d->num_snapshots; k++)
        if (widget == d->snapshot[k].button || widget == d->snapshot[k].restore_button)
            return k;

    return -1;
}

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    DT_TRY_GUI_UPDATE();

    d->selected = -1;

    /* check if snapshot is activated */
    if (gtk_toggle_button_get_active(widget))
    {
        d->selected = _lib_snapshots_get_activated(self, GTK_WIDGET(widget));

        /* lets deactivate all togglebuttons except for self */
        for (uint32_t k = 0; k < d->num_snapshots; k++)
            if (d->selected != k)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);
    }
    darktable.lib->proxy.snapshots.enabled = d->selected >= 0;
    gtk_widget_set_visible(dt_ui_snapshot(darktable.gui->ui), d->sidebyside && d->selected >= 0);

    DT_LEAVE_GUI_UPDATE();

    /* redraw center view */
    dt_control_queue_redraw_center();
}

static void _lib_snapshots_restore_callback(GtkButton *widget, dt_lib_module_t *self)
{
    dt_lib_snapshots_t *d = self->data;

    const int restore_idx = _lib_snapshots_get_activated(self, GTK_WIDGET(widget));

    dt_lib_snapshot_t *s = &d->snapshot[restore_idx];

    const dt_imgid_t imgid = s->imgid;

    dt_history_snapshot_restore(imgid, s->id, s->history_end);

    dt_dev_undo_start_record(darktable.develop);

    // reload history and set back snapshot history end
    dt_dev_reload_history_items(darktable.develop);

    dt_dev_pixelpipe_rebuild(darktable.develop);
    darktable.develop->history_end = s->history_end;
    dt_dev_pop_history_items(darktable.develop, darktable.develop->history_end);
    dt_ioppr_resync_modules_order(darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
    dt_image_update_final_size(imgid);
    dt_dev_write_history(darktable.develop);

    /* signal history changed */
    dt_dev_undo_end_record(darktable.develop);
}
