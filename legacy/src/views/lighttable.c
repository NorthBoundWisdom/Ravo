/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.
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
/** this is the view for the lighttable module.  */

#include "common/extra_optimizations.h"

#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "dtgtk/culling.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/view_api.h"


#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

DT_MODULE(1)

typedef enum dt_lighttable_loupe_source_t
{
    DT_LIGHTTABLE_LOUPE_NONE = 0,
    DT_LIGHTTABLE_LOUPE_PREVIEW
} dt_lighttable_loupe_source_t;

/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
    // culling and preview struct.
    dt_culling_t *culling;
    dt_culling_t *preview;

    dt_lighttable_layout_t current_layout;

    int preview_sticky;                        // are we in sticky preview mode
    dt_lighttable_loupe_source_t loupe_source; // explicit single-image preview state
    gboolean already_started; // is it the first start of lighttable. Used by culling
    int thumbtable_offset;    // last thumbtable offset before entering culling
} dt_library_t;

static gboolean _loupe_active(const dt_library_t *lib)
{
    return lib->loupe_source != DT_LIGHTTABLE_LOUPE_NONE;
}

static gboolean _explicit_preview(const dt_library_t *lib)
{
    return lib->loupe_source == DT_LIGHTTABLE_LOUPE_PREVIEW;
}

const char *name(const dt_view_t *self)
{
    return _("lighttable");
}

uint32_t view(const dt_view_t *self)
{
    return DT_VIEW_LIGHTTABLE;
}

static void _show_filmstrip(void)
{
    dt_lib_module_t *filmstrip = darktable.view_manager->proxy.filmstrip.module;
    if (!filmstrip)
        return;

    dt_lib_set_visible(filmstrip, TRUE);
    gtk_widget_queue_draw(filmstrip->widget);
}

// Exit Loupe and restore the underlying layout.
static void _loupe_quit(dt_view_t *self)
{
    dt_library_t *lib = self->data;
    const dt_imgid_t current_imgid = lib->preview->offset_imgid;

    dt_culling_set_hand_tool(lib->preview, FALSE);
    dt_culling_zoom_end(lib->preview);
    dt_culling_zoom_fit(lib->preview);
    gtk_widget_hide(lib->preview->widget);
    if (lib->preview->selection_sync && dt_is_valid_imgid(current_imgid))
    {
        dt_selection_select_single(darktable.selection, current_imgid);
    }
    lib->loupe_source = DT_LIGHTTABLE_LOUPE_NONE;
    dt_view_lighttable_update_layout_buttons(darktable.view_manager);
    // restore panels
    dt_ui_restore_panels(darktable.gui->ui);

    // The bottom photo browser remains available for every Lighttable layout.
    if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
        lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        // update thumbtable, to indicate if we navigate inside selection or not
        // this is needed as collection change is handle there
        dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection =
            lib->culling->navigate_inside_selection;

        _show_filmstrip();

        dt_culling_update_active_images_list(lib->culling);
    }
    else
    {
        dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = FALSE;
        _show_filmstrip();

        // restore the grid thumbtable
        dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui),
                                 dt_ui_center_base(darktable.gui->ui),
                                 DT_THUMBTABLE_MODE_FILEMANAGER);
        if (dt_is_valid_imgid(current_imgid))
            dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), current_imgid,
                                           FALSE);
        else
            dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), lib->thumbtable_offset,
                                     FALSE);
        gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
        dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);
        if (dt_is_valid_imgid(current_imgid))
            dt_thumbtable_ensure_imgid_visibility(dt_ui_thumbtable(darktable.gui->ui),
                                                  current_imgid);
    }
}

// check if we need to change the layout, and apply the change if needed
static void _lighttable_check_layout(dt_view_t *self)
{
    dt_library_t *lib = self->data;
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);
    const dt_lighttable_layout_t layout_old = lib->current_layout;

    if (lib->current_layout == layout)
        return;

    // A layout change always ends single-image Loupe.
    if (_loupe_active(lib))
        _loupe_quit(self);

    lib->current_layout = layout;

    // layout has changed, let restore panels
    dt_ui_restore_panels(darktable.gui->ui);

    if (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
        dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection = FALSE;
        gtk_widget_hide(lib->preview->widget);
        gtk_widget_hide(lib->culling->widget);
        gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);

        // if we arrive from culling, we just need to ensure the offset is right
        if (layout_old == DT_LIGHTTABLE_LAYOUT_CULLING ||
            layout_old == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
        {
            dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), lib->thumbtable_offset,
                                     FALSE);
        }
        // we want to reacquire the thumbtable if needed
        dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui),
                                 dt_ui_center_base(darktable.gui->ui),
                                 DT_THUMBTABLE_MODE_FILEMANAGER);
        dt_thumbtable_full_redraw(dt_ui_thumbtable(darktable.gui->ui), TRUE);
        gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
    }
    else if (layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
             layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        // record thumbtable offset
        lib->thumbtable_offset = dt_thumbtable_get_offset(dt_ui_thumbtable(darktable.gui->ui));
        dt_lighttable_culling_restriction_t restriction = DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO;

        if (layout == DT_LIGHTTABLE_LAYOUT_CULLING)
            restriction = dt_view_lighttable_culling_initial_restriction(darktable.view_manager);

        if (!lib->already_started)
        {
            int id = lib->thumbtable_offset;
            sqlite3_stmt *stmt;
            gchar *query = g_strdup_printf("SELECT rowid"
                                           " FROM memory.collected_images"
                                           " WHERE imgid=%d",
                                           dt_conf_get_int("plugins/lighttable/culling_last_id"));
            DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
                id = sqlite3_column_int(stmt, 0);
            }
            g_free(query);
            sqlite3_finalize(stmt);

            dt_culling_init(lib->culling, id, restriction);
        }
        else
            dt_culling_init(lib->culling, lib->thumbtable_offset, restriction);

        // ensure that thumbtable is not visible in the main view
        gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
        gtk_widget_hide(lib->preview->widget);
        gtk_widget_show(lib->culling->widget);

        dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection =
            lib->culling->navigate_inside_selection;
        dt_view_lighttable_update_layout_buttons(darktable.view_manager);
    }

    lib->already_started = TRUE;

    if (layout == DT_LIGHTTABLE_LAYOUT_CULLING || layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui),
                                 dt_ui_center_base(darktable.gui->ui), DT_THUMBTABLE_MODE_NONE);
        _show_filmstrip();
        dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
        dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui),
                                       lib->culling->offset_imgid, TRUE);
        dt_culling_update_active_images_list(lib->culling);
    }
    else
    {
        _show_filmstrip();
    }
}

static void _loupe_enter(dt_view_t *self, const dt_lighttable_loupe_source_t source,
                         const gboolean sticky, const gboolean focus,
                         const dt_lighttable_culling_restriction_t restriction)
{
    dt_library_t *lib = self->data;
    const gboolean was_active = _loupe_active(lib);

    if (!was_active)
        lib->thumbtable_offset = dt_thumbtable_get_offset(dt_ui_thumbtable(darktable.gui->ui));

    gtk_widget_hide(dt_ui_thumbtable(darktable.gui->ui)->widget);
    gtk_widget_hide(lib->culling->widget);

    dt_culling_set_hand_tool(lib->preview, FALSE);
    dt_culling_zoom_fit(lib->preview);
    dt_culling_set_hover_enabled(lib->preview, source != DT_LIGHTTABLE_LOUPE_PREVIEW);
    lib->preview_sticky = sticky;
    lib->preview->focus = focus;
    lib->loupe_source = source;
    dt_culling_init(lib->preview, was_active ? lib->preview->offset : lib->thumbtable_offset,
                    restriction);
    dt_view_lighttable_update_layout_buttons(darktable.view_manager);
    gtk_widget_show(lib->preview->widget);
    // The single-image canvas is a zoom target rather than a hoverable
    // thumbnail. Set its cursor after realization so it is visible on entry.
    dt_culling_set_hand_tool(lib->preview, FALSE);

    dt_ui_thumbtable(darktable.gui->ui)->navigate_inside_selection =
        lib->preview->navigate_inside_selection;

    // The main thumbtable stays detached while the dedicated bottom browser
    // follows the active Loupe image.
    dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui),
                             dt_ui_center_base(darktable.gui->ui), DT_THUMBTABLE_MODE_NONE);
    _show_filmstrip();
    dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), lib->preview->offset_imgid,
                                   TRUE);

    dt_culling_update_active_images_list(lib->preview);

    dt_ui_restore_panels(darktable.gui->ui);
    if (source == DT_LIGHTTABLE_LOUPE_PREVIEW)
    {
        // Explicit Loupe is the persistent single-image workspace. Make its
        // chrome visible temporarily; the user's saved panel choices remain
        // unchanged and are restored on return to Grid.
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, TRUE, FALSE);
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, TRUE, FALSE);
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, TRUE, FALSE);
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, TRUE, FALSE);
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, TRUE, FALSE);
    }
    dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
}

static void _lighttable_change_offset(dt_view_t *self, const gboolean reset, const dt_imgid_t imgid)
{
    dt_library_t *lib = self->data;

    // single-image Loupe/Preview change
    if (_loupe_active(lib))
    {
        // we only do the change if the offset is different
        if (lib->preview->offset_imgid != imgid)
            dt_culling_change_offset_image(lib->preview, imgid);
    }

    // culling change (note that full_preview can be combined with culling)
    if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
        lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        dt_culling_change_offset_image(lib->culling, imgid);
    }
}

static void _culling_preview_reload_overlays(dt_view_t *self)
{
    dt_library_t *lib = self->data;

    // change overlays if needed for culling and preview
    gchar *otxt =
        g_strdup_printf("plugins/lighttable/overlays/culling/%d", DT_CULLING_MODE_CULLING);
    dt_thumbnail_overlay_t over = dt_conf_get_int(otxt);
    dt_culling_set_overlays_mode(lib->culling, over);
    g_free(otxt);
    otxt = g_strdup_printf("plugins/lighttable/overlays/culling/%d", DT_CULLING_MODE_PREVIEW);
    over = dt_conf_get_int(otxt);
    dt_culling_set_overlays_mode(lib->preview, over);
    g_free(otxt);
}

static void _culling_preview_refresh(dt_view_t *self)
{
    dt_library_t *lib = self->data;

    // change overlays if needed for culling and preview
    _culling_preview_reload_overlays(self);

    // single-image Loupe/Preview change
    if (_loupe_active(lib))
    {
        // A collection refresh can replace the current image without changing
        // its row offset (for example after deleting it). Treat that as an
        // image switch so the replacement cannot inherit Loupe interaction.
        dt_culling_set_hand_tool(lib->preview, FALSE);
        dt_culling_zoom_end(lib->preview);
        dt_culling_zoom_fit(lib->preview);
        dt_culling_full_redraw(lib->preview, TRUE);
    }

    // culling change (note that full_preview can be combined with culling)
    if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
        lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        dt_culling_full_redraw(lib->culling, TRUE);
    }
}

static gboolean _preview_get_state(dt_view_t *self)
{
    dt_library_t *lib = self->data;
    return _explicit_preview(lib);
}

static dt_imgid_t _culling_get_selection(dt_view_t *self)
{
    const dt_library_t *lib = self->data;

    // we only return a value in culling layout
    if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
        lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        return lib->culling->selection;
    }
    return NO_IMGID;
}

static void _culling_restricted_set_state(dt_view_t *self,
                                          const dt_lighttable_culling_restriction_t state)
{
    dt_library_t *lib = self->data;
    if (_explicit_preview(lib))
    {
        lib->preview->navigate_inside_selection =
            (state == DT_LIGHTTABLE_CULLING_RESTRICTION_SELECTION);
    }
    else if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
        lib->culling->navigate_inside_selection =
            (state == DT_LIGHTTABLE_CULLING_RESTRICTION_SELECTION);
    }
}

static dt_lighttable_culling_restriction_t _culling_restricted_get_state(dt_view_t *self)
{
    const dt_library_t *lib = self->data;
    gboolean inside = FALSE;
    if (_explicit_preview(lib))
    {
        inside = lib->preview->navigate_inside_selection;
    }
    else if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
        inside = lib->culling->navigate_inside_selection;
    }

    if (inside)
        return DT_LIGHTTABLE_CULLING_RESTRICTION_SELECTION;
    else
        return DT_LIGHTTABLE_CULLING_RESTRICTION_COLLECTION;
}


void cleanup(dt_view_t *self)
{
    dt_library_t *lib = self->data;
    dt_culling_set_hand_tool(lib->preview, FALSE);
    free(lib->culling);
    free(lib->preview);
    free(self->data);
}

void expose(dt_view_t *self, cairo_t *cr, const int32_t width, const int32_t height,
            const int32_t pointerx, const int32_t pointery)
{
    dt_library_t *lib = self->data;

    const double start = dt_get_debug_wtime();
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

    // Apply layout changes before exposing the active Grid, Loupe, or Culling widget.
    _lighttable_check_layout(self);

    if (!darktable.collection || dt_collection_get_count_no_group(darktable.collection) <= 0)
    {
        // thumbtable displays an help message
    }
    else if (_loupe_active(lib))
    {
        if (!gtk_widget_get_visible(lib->preview->widget))
            gtk_widget_show(lib->preview->widget);
        gtk_widget_hide(lib->culling->widget);
    }
    else // we do pass on expose to filemanager
    {
        switch (layout)
        {
        case DT_LIGHTTABLE_LAYOUT_FILEMANAGER:
            if (!gtk_widget_get_visible(dt_ui_thumbtable(darktable.gui->ui)->widget))
                gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
            break;
        case DT_LIGHTTABLE_LAYOUT_CULLING:
        case DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC:
            if (!gtk_widget_get_visible(lib->culling->widget))
                gtk_widget_show(lib->culling->widget);
            gtk_widget_hide(lib->preview->widget);
            break;
        case DT_LIGHTTABLE_LAYOUT_PREVIEW:
            break;
        case DT_LIGHTTABLE_LAYOUT_FIRST:
        case DT_LIGHTTABLE_LAYOUT_LAST:
            break;
        }
    }

    // we have started the first expose
    lib->already_started = TRUE;

    dt_print(DT_DEBUG_LIGHTTABLE | DT_DEBUG_PERF, "[lighttable] expose took %0.04f sec",
             dt_get_wtime() - start);
}

void enter(dt_view_t *self)
{
    dt_library_t *lib = self->data;
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

    dt_start_backthumbs_crawler();
    // enable culling proxy
    darktable.view_manager->proxy.lighttable.culling_preview_refresh = _culling_preview_refresh;
    darktable.view_manager->proxy.lighttable.culling_preview_reload_overlays =
        _culling_preview_reload_overlays;

    // we want to reacquire the thumbtable if needed
    if (!_loupe_active(lib))
    {
        if (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        {
            dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui),
                                     dt_ui_center_base(darktable.gui->ui),
                                     DT_THUMBTABLE_MODE_FILEMANAGER);
            gtk_widget_show(dt_ui_thumbtable(darktable.gui->ui)->widget);
        }
    }

    // clean the undo list
    dt_undo_clear(darktable.undo, DT_UNDO_LIGHTTABLE);

    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

    dt_collection_hint_message(darktable.collection);

    // The bottom photo browser remains available for every Lighttable layout.
    if (layout == DT_LIGHTTABLE_LAYOUT_CULLING || layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC ||
        _loupe_active(lib))
    {
        _show_filmstrip();

        if (_loupe_active(lib))
            dt_culling_update_active_images_list(lib->preview);
        else
            dt_culling_update_active_images_list(lib->culling);
    }
    else
    {
        _show_filmstrip();
    }

    // restore panels
    dt_ui_restore_panels(darktable.gui->ui);
}

static void _preview_set_state(dt_view_t *self, const gboolean state, const gboolean sticky,
                               const gboolean focus,
                               const dt_lighttable_culling_restriction_t restriction)
{
    dt_library_t *lib = self->data;
    if (state)
        _loupe_enter(self, DT_LIGHTTABLE_LOUPE_PREVIEW, sticky, focus, restriction);
    else if (_loupe_active(lib))
        _loupe_quit(self);
}

void init(dt_view_t *self)
{
    self->data = calloc(1, sizeof(dt_library_t));
    dt_library_t *lib = self->data;

    lib->current_layout = DT_LIGHTTABLE_LAYOUT_FIRST;

    darktable.view_manager->proxy.lighttable.get_preview_state = _preview_get_state;
    darktable.view_manager->proxy.lighttable.set_preview_state = _preview_set_state;
    darktable.view_manager->proxy.lighttable.get_culling_restricted_state =
        _culling_restricted_get_state;
    darktable.view_manager->proxy.lighttable.set_culling_restricted_state =
        _culling_restricted_set_state;
    darktable.view_manager->proxy.lighttable.get_culling_selection = _culling_get_selection;
    darktable.view_manager->proxy.lighttable.view = self;
    darktable.view_manager->proxy.lighttable.change_offset = _lighttable_change_offset;

    dt_conf_remove_key("plugins/lighttable/timeline/last_zoom");

    // ensure the memory table is up to date
    dt_collection_memory_update();

}

void leave(dt_view_t *self)
{
    dt_stop_backthumbs_crawler(FALSE);
    dt_library_t *lib = self->data;

    // disable culling proxy
    darktable.view_manager->proxy.lighttable.culling_preview_refresh = NULL;
    darktable.view_manager->proxy.lighttable.culling_preview_reload_overlays = NULL;

    // ensure we have no active image remaining
    if (darktable.view_manager->active_images)
    {
        g_slist_free(darktable.view_manager->active_images);
        darktable.view_manager->active_images = NULL;
        DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
    }

    // we hide culling and preview too
    dt_culling_set_hand_tool(lib->preview, FALSE);
    dt_culling_zoom_end(lib->preview);
    dt_culling_zoom_fit(lib->preview);
    gtk_widget_hide(lib->culling->widget);
    gtk_widget_hide(lib->preview->widget);

    // exit preview mode if non-sticky
    if (_explicit_preview(lib) && lib->preview_sticky == 0)
    {
        _loupe_quit(self);
    }

    // we remove the thumbtable from main view
    dt_thumbtable_set_parent(dt_ui_thumbtable(darktable.gui->ui), NULL, DT_THUMBTABLE_MODE_NONE);

    dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
}

void reset(dt_view_t *self)
{
    dt_library_t *lib = self->data;
    dt_culling_set_hand_tool(lib->preview, FALSE);
    dt_control_set_mouse_over_id(NO_IMGID);
}

// Return the active dt_culling_t for gesture dispatch: the single-image widget
// for Loupe, or the culling widget for a culling layout.
static dt_culling_t *_active_culling(const dt_library_t *lib)
{
    if (_loupe_active(lib))
    {
        return lib->preview;
    }

    if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
        lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        return lib->culling;
    }

    return NULL;
}

gboolean gesture_pan(dt_view_t *self, const double x, const double y, const double dx,
                     const double dy, const int state)
{
    const dt_library_t *lib = self->data;
    dt_culling_t *table = _active_culling(lib);
    dt_print(DT_DEBUG_INPUT,
             "[lighttable pan] x=%.1f y=%.1f dx=%.3f dy=%.3f state=0x%x"
             " layout=%d loupe=%d table=%s",
             x, y, dx, dy, state, lib->current_layout, lib->loupe_source,
             table ? "active" : "NULL (not in culling/preview)");
    if (!table)
        return FALSE;

    const gboolean moved = dt_culling_pan_move(table, (float)dx, (float)dy, state);
    dt_print(DT_DEBUG_INPUT, "[lighttable pan] dt_culling_pan_move -> %s",
             moved ? "moved" : "no-op");
    if (moved)
    {
        gtk_widget_queue_draw(table->widget);
    }

    return moved;
}

gboolean gesture_pinch(dt_view_t *self, const double x, const double y, const double dx,
                       const double dy, const int phase, const double scale, const int state)
{
    const dt_library_t *lib = self->data;
    dt_culling_t *table = _active_culling(lib);
    if (!table)
    {
        return FALSE;
    }

    // prev_scale tracks the cumulative scale from the last UPDATE so we can compute
    // an incremental scale ratio each event rather than needing per-thumbnail begin state.
    static double prev_scale = 1.0;

    dt_print(DT_DEBUG_INPUT,
             "[lighttable pinch] phase=%d x=%.1f y=%.1f dx=%.3f dy=%.3f"
             " scale=%.6f prev_scale=%.6f state=0x%x layout=%d loupe=%d",
             phase, x, y, dx, dy, scale, prev_scale, state, lib->current_layout, lib->loupe_source);

    if (phase == GDK_TOUCHPAD_GESTURE_PHASE_BEGIN)
    {
        prev_scale = 1.0;
        dt_print(DT_DEBUG_INPUT, "[lighttable pinch] begin -> reset prev_scale");
        return TRUE;
    }
    if (phase == GDK_TOUCHPAD_GESTURE_PHASE_END || phase == GDK_TOUCHPAD_GESTURE_PHASE_CANCEL)
    {
        prev_scale = 1.0;
        dt_print(DT_DEBUG_INPUT, "[lighttable pinch] %s",
                 phase == GDK_TOUCHPAD_GESTURE_PHASE_END ? "end" : "cancel");
        // Gesture is done: reload surfaces at the correct zoom resolution now.
        dt_culling_zoom_end(table);
        return TRUE;
    }
    if (phase != GDK_TOUCHPAD_GESTURE_PHASE_UPDATE)
    {
        dt_print(DT_DEBUG_INPUT, "[lighttable pinch] unknown phase %d -> ignored", phase);
        return FALSE;
    }

    gboolean changed = FALSE;

    // pan component (combined pinch+translation, from GdkEventTouchpadPinch dx/dy)
    // Negate dx/dy so the gesture feels like scrolling (moving fingers right shifts the
    // viewport right, i.e. the image moves left) rather than touchscreen dragging.
    if (dx != 0.0 || dy != 0.0)
    {
        dt_print(DT_DEBUG_INPUT, "[lighttable pinch] pan component dx=%.3f dy=%.3f", dx, dy);
        changed |= dt_culling_pan_move(table, (float)-dx, (float)-dy, state);
    }

    // zoom component — derive an incremental zoom_delta from the scale ratio.
    // Tuning: a full 2× pinch spread (cumulative scale 1.0→2.0) should cover most of
    // the fit-to-100% range. sum of (scale/prev_scale - 1) over a smooth 2× pinch
    // ≈ ln(2) ≈ 0.69, and zoom_100 - 1 ≈ 3–5, giving SPEED ≈ 5–7.
    //
    // Always advance prev_scale so the dead zone is measured against the immediately
    // preceding event rather than the last zoom-fire point. This keeps the worst-case
    // accumulated noise to a single event step (~0.4%), well within the 1% threshold.
    const float scale_increment = (float)(scale / prev_scale) - 1.0f;
    prev_scale = scale;
    if (fabsf(scale_increment) > 0.01f)
    {
        const float zoom_delta = scale_increment * 5.0f;
        dt_print(DT_DEBUG_INPUT,
                 "[lighttable pinch] zoom scale_increment=%.6f zoom_delta=%.4f"
                 " x_root=%.1f y_root=%.1f",
                 scale_increment, zoom_delta, x, y);

        if (dt_culling_zoom_add(table, zoom_delta, x, y, state))
        {
            changed = TRUE;
        }
        else
        {
            dt_print(DT_DEBUG_INPUT, "[lighttable pinch] dt_culling_zoom_add -> no-op");
        }
    }

    prev_scale = scale;
    dt_print(DT_DEBUG_INPUT, "[lighttable pinch] update done changed=%d", changed);
    if (changed)
    {
        gtk_widget_queue_draw(table->widget);
    }

    return TRUE;
}

void scrollbar_changed(dt_view_t *self, const double x, const double y)
{
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

    const dt_library_t *lib = self->data;
    if (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && !_loupe_active(lib))
        dt_thumbtable_scrollbar_changed(dt_ui_thumbtable(darktable.gui->ui), x, y);
}

static void _overlays_force(dt_view_t *self, const gboolean show)
{
    dt_library_t *lib = self->data;

    // Keep the state on both renderers even while their widgets are hidden so
    // newly-created thumbnails inherit it on the next Loupe/Culling entry.
    dt_culling_force_overlay(lib->preview, show);
    dt_culling_force_overlay(lib->culling, show);
}

static float _action_process_infos(gpointer target, const dt_action_element_t element,
                                   const dt_action_effect_t effect, const float move_size)
{
    dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
    gboolean pinned = dt_conf_get_bool("plugins/lighttable/info_overlay_pinned");

    if (DT_ACTION_TOGGLE_NEEDED(effect, move_size, pinned))
    {
        pinned = effect == DT_ACTION_EFFECT_ON  ? TRUE :
                 effect == DT_ACTION_EFFECT_OFF ? FALSE :
                                                  !pinned;
        dt_conf_set_bool("plugins/lighttable/info_overlay_pinned", pinned);
        _overlays_force(self, pinned);
    }

    return pinned;
}

const dt_action_element_def_t _action_elements_infos[] = {{NULL, dt_action_effect_toggle}, {NULL}};

const dt_action_def_t dt_action_def_infos = {N_("show infos"), _action_process_infos,
                                             _action_elements_infos, NULL, TRUE};

enum
{
    DT_ACTION_ELEMENT_MOVE = 0,
    DT_ACTION_ELEMENT_SELECT = 1,
};

enum
{
    _ACTION_TABLE_MOVE_STARTEND = 0,
    _ACTION_TABLE_MOVE_LEFTRIGHT = 1,
    _ACTION_TABLE_MOVE_UPDOWN = 2,
    _ACTION_TABLE_MOVE_PAGE = 3,
    _ACTION_TABLE_MOVE_LEAVE = 4,
};

static float _action_process_move(gpointer target, const dt_action_element_t element,
                                  const dt_action_effect_t effect, const float move_size)
{
    if (!DT_PERFORM_ACTION(move_size))
        return 0; // FIXME return should be relative position

    const int action = GPOINTER_TO_INT(target);

    dt_library_t *lib = darktable.view_manager->proxy.lighttable.view->data;
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

    // navigation accels for thumbtable layouts this can't be "normal"
    // key accels because it's usually arrow keys and lot of other
    // widgets will capture them before the usual accel is triggered
    if (!_loupe_active(lib) && layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
        dt_thumbtable_move_t move = DT_THUMBTABLE_MOVE_NONE;
        const gboolean select = element == DT_ACTION_ELEMENT_SELECT;
        if (action == _ACTION_TABLE_MOVE_LEFTRIGHT && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_THUMBTABLE_MOVE_LEFT;
        else if (action == _ACTION_TABLE_MOVE_UPDOWN && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_THUMBTABLE_MOVE_UP;
        else if (action == _ACTION_TABLE_MOVE_LEFTRIGHT && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_THUMBTABLE_MOVE_RIGHT;
        else if (action == _ACTION_TABLE_MOVE_UPDOWN && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_THUMBTABLE_MOVE_DOWN;
        else if (action == _ACTION_TABLE_MOVE_PAGE && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_THUMBTABLE_MOVE_PAGEUP;
        else if (action == _ACTION_TABLE_MOVE_PAGE && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_THUMBTABLE_MOVE_PAGEDOWN;
        else if (action == _ACTION_TABLE_MOVE_STARTEND && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_THUMBTABLE_MOVE_START;
        else if (action == _ACTION_TABLE_MOVE_STARTEND && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_THUMBTABLE_MOVE_END;
        else if (action == _ACTION_TABLE_MOVE_LEAVE && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_THUMBTABLE_MOVE_LEAVE;
        else
        {
            // MIDDLE
        }

        if (move != DT_THUMBTABLE_MOVE_NONE)
        {
            // for this layout navigation keys are managed directly by thumbtable
            dt_thumbtable_key_move(dt_ui_thumbtable(darktable.gui->ui), move, select);
            gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
        }
    }
    else if (_loupe_active(lib) || layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
             layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        dt_culling_move_t move = DT_CULLING_MOVE_NONE;
        if (action == _ACTION_TABLE_MOVE_LEFTRIGHT && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_CULLING_MOVE_LEFT;
        else if (action == _ACTION_TABLE_MOVE_UPDOWN && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_CULLING_MOVE_UP;
        else if (action == _ACTION_TABLE_MOVE_LEFTRIGHT && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_CULLING_MOVE_RIGHT;
        else if (action == _ACTION_TABLE_MOVE_UPDOWN && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_CULLING_MOVE_DOWN;
        else if (action == _ACTION_TABLE_MOVE_PAGE && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_CULLING_MOVE_PAGEUP;
        else if (action == _ACTION_TABLE_MOVE_PAGE && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_CULLING_MOVE_PAGEDOWN;
        else if (action == _ACTION_TABLE_MOVE_STARTEND && effect == DT_ACTION_EFFECT_PREVIOUS)
            move = DT_CULLING_MOVE_START;
        else if (action == _ACTION_TABLE_MOVE_STARTEND && effect == DT_ACTION_EFFECT_NEXT)
            move = DT_CULLING_MOVE_END;

        if (move != DT_CULLING_MOVE_NONE)
        {
            // for this layout navigation keys are managed directly by thumbtable
            if (_loupe_active(lib))
                dt_culling_key_move(lib->preview, move);
            else
                dt_culling_key_move(lib->culling, move);
            gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
        }
    }

    return 0; // FIXME return should be relative position
}

const gchar *_action_effect_move[] = {N_("middle"), N_("next"), N_("previous"), NULL};

const dt_action_element_def_t _action_elements_move[] = {{N_("move"), _action_effect_move},
                                                         {N_("select"), _action_effect_move},
                                                         {NULL}};

static const dt_shortcut_fallback_t _action_fallbacks_move[] = {
    {.mods = GDK_SHIFT_MASK, .element = DT_ACTION_ELEMENT_SELECT},
    {}};

const dt_action_def_t _action_def_move = {N_("move"), _action_process_move, _action_elements_move,
                                          _action_fallbacks_move, TRUE};

static void zoom_in_callback(dt_action_t *action)
{
    int zoom = dt_view_lighttable_get_zoom(darktable.view_manager);

    if (dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        zoom += DT_LIGHTTABLE_GRID_THUMBNAIL_SIZE_STEP;
    else
        zoom--;

    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
}

static void zoom_out_callback(dt_action_t *action)
{
    int zoom = dt_view_lighttable_get_zoom(darktable.view_manager);

    if (dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        zoom -= DT_LIGHTTABLE_GRID_THUMBNAIL_SIZE_STEP;
    else
        zoom++;

    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
}

static void zoom_max_callback(dt_action_t *action)
{
    const int zoom = dt_view_lighttable_get_layout(darktable.view_manager) ==
                             DT_LIGHTTABLE_LAYOUT_FILEMANAGER
                         ? DT_LIGHTTABLE_GRID_MAX_THUMBNAIL_SIZE
                         : 1;
    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
}

static void zoom_min_callback(dt_action_t *action)
{
    const int zoom = dt_view_lighttable_get_layout(darktable.view_manager) ==
                             DT_LIGHTTABLE_LAYOUT_FILEMANAGER
                         ? DT_LIGHTTABLE_GRID_MIN_THUMBNAIL_SIZE
                         : DT_LIGHTTABLE_MAX_ZOOM;
    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
}

static void _lighttable_undo_callback(dt_action_t *action)
{
    dt_undo_do_undo(darktable.undo, DT_UNDO_LIGHTTABLE);
}

static void _lighttable_redo_callback(dt_action_t *action)
{
    dt_undo_do_redo(darktable.undo, DT_UNDO_LIGHTTABLE);
}

static void _accel_reset_first_offset(dt_action_t *action)
{
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

    if (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        dt_thumbtable_reset_first_offset(dt_ui_thumbtable(darktable.gui->ui));
}

static void _accel_culling_zoom_100(dt_action_t *action)
{
    dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
    dt_library_t *lib = self->data;
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

    if (_loupe_active(lib))
        dt_culling_zoom_max(lib->preview);
    else if (layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
             layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
        dt_culling_zoom_max(lib->culling);
}

static void _accel_culling_zoom_fit(dt_action_t *action)
{
    dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
    dt_library_t *lib = self->data;
    const dt_lighttable_layout_t layout = dt_view_lighttable_get_layout(darktable.view_manager);

    if (_loupe_active(lib))
        dt_culling_zoom_fit(lib->preview);
    else if (layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
             layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
        dt_culling_zoom_fit(lib->culling);
}

static void _accel_culling_zoom_toggle(dt_action_t *action)
{
    dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
    dt_library_t *lib = self->data;

    _lighttable_check_layout(self);

    dt_culling_t *table = _active_culling(lib);
    if (!table && lib->current_layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
        _loupe_enter(self, DT_LIGHTTABLE_LOUPE_PREVIEW, TRUE, FALSE,
                     DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO);
        dt_culling_full_redraw(lib->preview, TRUE);
        dt_culling_zoom_toggle(lib->preview);
        return;
    }

    dt_culling_zoom_toggle(table);
}

static void _accel_select_toggle(dt_action_t *action)
{
    const dt_imgid_t id = dt_control_get_mouse_over_id();
    dt_selection_toggle(darktable.selection, id);
}

static float _action_process_select_or_hand(gpointer target, const dt_action_element_t element,
                                            const dt_action_effect_t effect, const float move_size)
{
    dt_library_t *lib = darktable.view_manager->proxy.lighttable.view->data;

    if (DT_PERFORM_ACTION(move_size))
    {
        if (_loupe_active(lib))
        {
            switch (effect)
            {
            case DT_ACTION_EFFECT_ON:
                dt_culling_set_hand_tool(lib->preview, TRUE);
                break;
            case DT_ACTION_EFFECT_OFF:
                dt_culling_set_hand_tool(lib->preview, FALSE);
                break;
            default:
                dt_culling_set_hand_tool(lib->preview, !lib->preview->hand_tool);
                break;
            }
        }
        else if (effect != DT_ACTION_EFFECT_OFF)
        {
            _accel_select_toggle(NULL);
        }
    }

    return _loupe_active(lib) ? lib->preview->hand_tool : 0.0f;
}

static const dt_action_def_t _action_def_select_or_hand = {N_("select image or hand tool"),
                                                           _action_process_select_or_hand,
                                                           dt_action_elements_hold, NULL, TRUE};

static void _accel_select_single(dt_action_t *action)
{
    const dt_imgid_t id = dt_control_get_mouse_over_id();
    dt_selection_select_single(darktable.selection, id);
}

GSList *mouse_actions(const dt_view_t *self)
{
    dt_library_t *lib = self->data;
    GSList *lm = NULL;

    if (_loupe_active(lib))
    {
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, 0, _("zoom to 100% and back"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT, 0,
                                           _("return to grid"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0,
                                           _("switch to next/previous image"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK,
                                           _("zoom in the image"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_MIDDLE,
                                           /* xgettext:no-c-format */
                                           0, _("zoom to 100% and back"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0,
                                           _("hold space to pan a zoomed image"));
    }
    else if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT, 0,
                                           _("open image in loupe"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0,
                                           _("scroll the collection"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK,
                                           _("change grid thumbnail size"));

        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, 0, _("select an image"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_SHIFT_MASK,
                                           _("select range from last image"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_CONTROL_MASK,
                                           _("add image to or remove it from a selection"));

        if (darktable.collection->params.sorts[DT_COLLECTION_SORT_CUSTOM_ORDER])
        {
            lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DRAG_DROP, GDK_BUTTON1_MASK,
                                               _("change image order"));
        }
    }
    else if (lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING ||
             lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
    {
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_DOUBLE_LEFT, 0,
                                           _("open image in darkroom"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0,
                                           _("scroll the collection"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK,
                                           _("zoom all the images"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0,
                                           _("pan inside all the images"));
        lm = dt_mouse_action_create_simple(
            lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK | GDK_SHIFT_MASK, _("zoom current image"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT_DRAG, GDK_SHIFT_MASK,
                                           _("pan inside current image"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_MIDDLE,
                                           /* xgettext:no-c-format */
                                           0, _("zoom to 100% and back"));
        lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_MIDDLE, GDK_SHIFT_MASK,
                                           /* xgettext:no-c-format */
                                           _("zoom current image to 100% and back"));
    }
    lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_SHIFT_MASK,
                                       dt_conf_get_bool("lighttable/ui/single_module") ?
                                           _("[modules] expand module without closing others") :
                                           _("[modules] expand module and close others"));

    return lm;
}

void gui_init(dt_view_t *self)
{
    dt_library_t *lib = self->data;

    lib->culling = dt_culling_new(DT_CULLING_MODE_CULLING);
    lib->preview = dt_culling_new(DT_CULLING_MODE_PREVIEW);
    _overlays_force(self, dt_conf_get_bool("plugins/lighttable/info_overlay_pinned"));

    // add culling and preview to the center widget
    gtk_overlay_add_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                            lib->culling->widget);
    gtk_overlay_add_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                            lib->preview->widget);
    gtk_widget_set_no_show_all(lib->culling->widget, TRUE);
    gtk_widget_set_no_show_all(lib->preview->widget, TRUE);
    // place behind toast/log messages
    gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                                lib->culling->widget, 1);
    gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                                lib->preview->widget, 1);

    dt_action_t *sa = &self->actions, *ac = NULL;

    ac = dt_action_define(sa, N_("move"), N_("whole"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_STARTEND),
                          &_action_def_move);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Home, 0);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT, GDK_KEY_End, 0);

    ac = dt_action_define(sa, N_("move"), N_("horizontal"),
                          GINT_TO_POINTER(_ACTION_TABLE_MOVE_LEFTRIGHT), &_action_def_move);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Left, 0);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT, GDK_KEY_Right, 0);

    ac = dt_action_define(sa, N_("move"), N_("vertical"),
                          GINT_TO_POINTER(_ACTION_TABLE_MOVE_UPDOWN), &_action_def_move);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Down, 0);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT, GDK_KEY_Up, 0);

    ac = dt_action_define(sa, N_("move"), N_("page"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_PAGE),
                          &_action_def_move);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_PREVIOUS, GDK_KEY_Page_Down,
                         0);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT, GDK_KEY_Page_Up, 0);

    ac = dt_action_define(sa, N_("move"), N_("leave"), GINT_TO_POINTER(_ACTION_TABLE_MOVE_LEAVE),
                          &_action_def_move);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_MOVE, DT_ACTION_EFFECT_NEXT, GDK_KEY_Escape,
                         GDK_MOD1_MASK);

    // Show infos key
    ac = dt_action_define(sa, NULL, N_("show infos"), NULL, &dt_action_def_infos);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_DEFAULT, DT_ACTION_EFFECT_TOGGLE, GDK_KEY_i, 0);

    dt_action_register(DT_ACTION(self), N_("reset first image offset"), _accel_reset_first_offset,
                       0, 0);
    ac = dt_action_define(sa, NULL, N_("select toggle image"), NULL, &_action_def_select_or_hand);
    dt_shortcut_register(ac, DT_ACTION_ELEMENT_DEFAULT, DT_ACTION_EFFECT_HOLD, GDK_KEY_space, 0);
    dt_action_register(DT_ACTION(self), N_("select single image"), _accel_select_single,
                       GDK_KEY_Return, 0);

    // undo/redo
    dt_action_register(DT_ACTION(self), N_("undo"), _lighttable_undo_callback, GDK_KEY_z,
                       GDK_CONTROL_MASK);
    dt_action_register(DT_ACTION(self), N_("redo"), _lighttable_redo_callback, GDK_KEY_y,
                       GDK_CONTROL_MASK);

    // zoom for full culling & preview
    dt_action_register(DT_ACTION(self), N_("preview zoom 100%"), _accel_culling_zoom_100, 0, 0);
    dt_action_register(DT_ACTION(self), N_("preview zoom fit"), _accel_culling_zoom_fit, 0, 0);
    dt_action_register(DT_ACTION(self), N_("preview zoom toggle"), _accel_culling_zoom_toggle,
                       GDK_KEY_z, 0);

    // zoom in/out/min/max
    dt_action_register(DT_ACTION(self), N_("zoom in"), zoom_in_callback, GDK_KEY_plus,
                       GDK_CONTROL_MASK);
    dt_action_register(DT_ACTION(self), N_("zoom max"), zoom_max_callback, GDK_KEY_plus,
                       GDK_MOD1_MASK);
    dt_action_register(DT_ACTION(self), N_("zoom out"), zoom_out_callback, GDK_KEY_minus,
                       GDK_CONTROL_MASK);
    dt_action_register(DT_ACTION(self), N_("zoom min"), zoom_min_callback, GDK_KEY_minus,
                       GDK_MOD1_MASK);
}
