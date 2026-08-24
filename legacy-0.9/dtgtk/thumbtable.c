/*
    This file is part of darktable,
    Copyright (C) 2024-2026 darktable developers.

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
/** a class to manage a table of thumbnail for lighttable and filmstrip.  */

#include "dtgtk/thumbtable.h"
#include "common/darktable.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/grouping.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "gui/accelerators.h"
#include "gui/context_menu.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "views/view.h"
#include "bauhaus/bauhaus.h"

#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

static void _list_remove_thumb(gpointer user_data)
{
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
    gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->w_main)), thumb->w_main);
    dt_thumbnail_destroy(thumb);
}

// get the class name associated with the overlays mode
static gchar *_thumbs_get_overlays_class(const dt_thumbnail_overlay_t over)
{
    switch (over)
    {
    case DT_THUMBNAIL_OVERLAYS_NONE:
        return g_strdup("dt_overlays_none");
    case DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED:
        return g_strdup("dt_overlays_hover_extended");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL:
        return g_strdup("dt_overlays_always");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED:
        return g_strdup("dt_overlays_always_extended");
    case DT_THUMBNAIL_OVERLAYS_MIXED:
        return g_strdup("dt_overlays_mixed");
    case DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK:
        return g_strdup("dt_overlays_hover_block");
    default:
        return g_strdup("dt_overlays_hover");
    }
}

// get the size category, depending on the thumb size
static int _thumbs_get_prefs_size(dt_thumbtable_t *table)
{
    // we get the size delimitations to differentiate sizes categories
    // one we set as many categories as we want (this can be useful if
    // we want to finetune css very precisely)
    const char *txt = dt_conf_get_string_const("plugins/lighttable/thumbnail_sizes");
    gchar **ts = g_strsplit(txt, "|", -1);
    int i = 0;
    while (ts[i])
    {
        const int s = g_ascii_strtoll(ts[i], NULL, 10);
        if (table->thumb_size < s)
            break;
        i++;
    }
    g_strfreev(ts);
    return i;
}

// update thumbtable class and overlays mode, depending on size category
static void _thumbs_update_overlays_mode(dt_thumbtable_t *table)
{
    const int ns = _thumbs_get_prefs_size(table);

    // we change the class that indicate the thumb size
    gchar *c0 = g_strdup_printf("dt_thumbnails_%d", table->prefs_size);
    gchar *c1 = g_strdup_printf("dt_thumbnails_%d", ns);
    dt_gui_remove_class(table->widget, c0);
    dt_gui_add_class(table->widget, c1);
    g_free(c0);
    g_free(c1);
    table->prefs_size = ns;

    // we change the overlay mode
    gchar *txt = g_strdup_printf("plugins/lighttable/overlays/%d/%d", table->mode, ns);
    dt_thumbnail_overlay_t over = dt_conf_get_int(txt);
    g_free(txt);
    txt = g_strdup_printf("plugins/lighttable/tooltips/%d/%d", table->mode, ns);
    table->show_tooltips = dt_conf_get_bool(txt);
    g_free(txt);

    dt_thumbtable_set_overlays_mode(table, over);
}

// change the type of overlays that should be shown
void dt_thumbtable_set_overlays_mode(dt_thumbtable_t *table, const dt_thumbnail_overlay_t over)
{
    if (!table)
        return;

    // The Grid is the primary browsing surface. Keep its file format, metadata,
    // and rating controls visible so scanning images does not depend on hover.
    const dt_thumbnail_overlay_t effective_over =
        table->mode == DT_THUMBTABLE_MODE_FILEMANAGER ? DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED
                                                      : over;

    // we ensure the tooltips change in any cases
    gchar *txt =
        g_strdup_printf("plugins/lighttable/tooltips/%d/%d", table->mode, table->prefs_size);
    dt_conf_set_bool(txt, table->show_tooltips);
    g_free(txt);

    int timeout = 2;
    if (effective_over != table->overlays)
    {
        // if the overlay change
        txt = g_strdup_printf("plugins/lighttable/overlays/%d/%d", table->mode, table->prefs_size);
        dt_conf_set_int(txt, effective_over);
        g_free(txt);
        gchar *cl0 = _thumbs_get_overlays_class(table->overlays);
        gchar *cl1 = _thumbs_get_overlays_class(effective_over);

        dt_gui_remove_class(table->widget, cl0);
        dt_gui_add_class(table->widget, cl1);
        g_free(cl0);
        g_free(cl1);

        txt = g_strdup_printf("plugins/lighttable/overlays_block_timeout/%d/%d", table->mode,
                              table->prefs_size);
        if (!dt_conf_key_exists(txt))
            timeout = dt_conf_get_int("plugins/lighttable/overlay_timeout");
        else
            timeout = dt_conf_get_int(txt);
        g_free(txt);
    }

    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        // in any cases, we update the tooltip
        th->tooltip = table->show_tooltips;

        if (effective_over != table->overlays)
        {
            // we need to change the overlay content if we pass from normal to
            // extended overlays this is not done on the fly with css to avoid
            // computing extended msg for nothing and to reserve space if needed
            dt_thumbnail_set_overlay(th, effective_over, timeout);
            // and we resize the bottom area
            dt_thumbnail_resize(th, th->width, th->height, TRUE, IMG_TO_FIT);
        }
        else
        {
            dt_thumbnail_update_infos(th);
        }
    }

    table->overlays = effective_over;
    table->overlays_block_timeout = timeout;
}

// change the type of overlays that should be shown
void dt_thumbtable_set_overlays_block_timeout(dt_thumbtable_t *table, const int timeout)
{
    if (!table)
        return;
    gchar *txt = g_strdup_printf("plugins/lighttable/overlays_block_timeout/%d/%d", table->mode,
                                 table->prefs_size);
    dt_conf_set_int(txt, timeout);
    g_free(txt);

    table->overlays_block_timeout = timeout;

    // we need to change the overlay timeout for each thumbnails
    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        th->overlay_timeout_duration = timeout;
    }
}

// get the thumb at specific position
static dt_thumbnail_t *_thumb_get_at_pos(dt_thumbtable_t *table, const int x, const int y)
{
    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        if (th->x <= x && th->x + th->width > x && th->y <= y && th->y + th->height > y)
            return th;
    }

    return NULL;
}

// get the thumb which is currently under mouse cursor
static dt_thumbnail_t *_thumb_get_under_mouse(dt_thumbtable_t *table)
{
    if (!table->mouse_inside)
        return NULL;

    int x = -1;
    int y = -1;
    gdk_window_get_origin(gtk_widget_get_window(table->widget), &x, &y);
    x = table->last_x - x;
    y = table->last_y - y;

    return _thumb_get_at_pos(table, x, y);
}

// get imgid from rowid
static dt_imgid_t _thumb_get_imgid(const int rowid)
{
    dt_imgid_t id = NO_IMGID;
    sqlite3_stmt *stmt;
    gchar *query =
        g_strdup_printf("SELECT imgid FROM memory.collected_images WHERE rowid=%d", rowid);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        id = sqlite3_column_int(stmt, 0);
    }
    g_free(query);
    sqlite3_finalize(stmt);
    return id;
}
// get rowid from imgid
static int _thumb_get_rowid(const dt_imgid_t imgid)
{
    int id = -1;
    sqlite3_stmt *stmt;
    gchar *query =
        g_strdup_printf("SELECT rowid FROM memory.collected_images WHERE imgid=%d", imgid);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        id = sqlite3_column_int(stmt, 0);
    }
    g_free(query);
    sqlite3_finalize(stmt);
    return id;
}

// get the coordinate of the rectangular area used by all the loaded thumbs
static void _pos_compute_area(dt_thumbtable_t *table)
{
    if (table->list)
    {
        int x1 = INT_MAX;
        int y1 = INT_MAX;
        int x2 = INT_MIN;
        int y2 = INT_MIN;
        for (const GList *l = table->list; l; l = g_list_next(l))
        {
            const dt_thumbnail_t *th = (const dt_thumbnail_t *)l->data;
            x1 = MIN(x1, th->x);
            y1 = MIN(y1, th->y);
            x2 = MAX(x2, th->x);
            y2 = MAX(y2, th->y);
        }

        table->thumbs_area.x = x1;
        table->thumbs_area.y = y1;
        table->thumbs_area.width = x2 + table->thumb_size - x1;
        table->thumbs_area.height = y2 + table->thumb_size - y1;
    }
    else
    {
        table->thumbs_area.x = 0;
        table->thumbs_area.y = 0;
        table->thumbs_area.width = 0;
        table->thumbs_area.height = 0;
    }
}

// get the position of the next image after the one at (x,y)
static void _pos_get_next(dt_thumbtable_t *table, int *x, int *y)
{
    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    {
        *x += table->thumb_size;
        if (*x + table->thumb_size > table->view_width)
        {
            *x = table->center_offset;
            *y += table->thumb_size;
        }
    }
    else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
        *x += table->thumb_size;
    }
}
// get the position of the previous image after the one at (x,y)
static void _pos_get_previous(dt_thumbtable_t *table, int *x, int *y)
{
    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    {
        *x -= table->thumb_size;
        if (*x < 0)
        {
            *x = (table->thumbs_per_row - 1) * table->thumb_size + table->center_offset;
            *y -= table->thumb_size;
        }
    }
    else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
        *x -= table->thumb_size;
    }
}

static int _grid_columns_for_width(const int width, const int target_size)
{
    const int size = CLAMP(target_size, DT_LIGHTTABLE_GRID_MIN_THUMBNAIL_SIZE,
                           DT_LIGHTTABLE_GRID_MAX_THUMBNAIL_SIZE);
    return CLAMP((width + size / 2) / size, DT_LIGHTTABLE_GRID_MIN_COLUMNS,
                 DT_LIGHTTABLE_GRID_MAX_COLUMNS);
}

// compute thumb_size, thumbs_per_row and rows for the current widget size
// return TRUE if something as changed (or forced) FALSE otherwise
static gboolean _compute_sizes(dt_thumbtable_t *table, const gboolean force)
{
    gboolean ret = FALSE; // return value to show if something as changed
    GtkAllocation allocation;
    gtk_widget_get_allocation(table->widget, &allocation);

    if (allocation.width <= 20 || allocation.height <= 20)
    {
        table->view_width = allocation.width;
        table->view_height = allocation.height;
        return FALSE;
    }

    const int old_size = table->thumb_size;
    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    {
        const int target_size = dt_view_lighttable_get_zoom(darktable.view_manager);
        const int npr = _grid_columns_for_width(allocation.width, target_size);

        if (force || allocation.width != table->view_width ||
            allocation.height != table->view_height || npr != table->thumbs_per_row)
        {
            table->thumbs_per_row = npr;
            table->view_width = allocation.width;
            table->view_height = allocation.height;
            table->thumb_size = MIN(table->view_width / table->thumbs_per_row, table->view_height);
            table->rows = table->view_height / table->thumb_size + 1;
            table->center_offset =
                (table->view_width - table->thumbs_per_row * table->thumb_size) / 2;
            ret = TRUE;
        }
    }
    else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
        if (force || allocation.width != table->view_width ||
            allocation.height != table->view_height)
        {
            table->thumbs_per_row = 1;
            table->view_width = allocation.width;
            table->view_height = allocation.height;
            table->thumb_size = table->view_height;
            table->rows = table->view_width / table->thumb_size;
            table->center_offset = 0;
            if (table->rows % 2)
                table->rows += 2;
            else
                table->rows += 1;
            ret = TRUE;
        }
    }
    // if the thumb size has changed, we need to set overlays, etc... correctly
    if (table->thumb_size != old_size)
    {
        _thumbs_update_overlays_mode(table);
    }
    return ret;
}

// update scrollbars positions and visibility
// return their visibility state
static gboolean _thumbtable_update_scrollbars(dt_thumbtable_t *table)
{
    if (table->mode != DT_THUMBTABLE_MODE_FILEMANAGER)
        return FALSE;
    if (!table->scrollbars)
        return FALSE;

    table->code_scrolling = TRUE;

    // get the total number of images
    const uint32_t nbid = MAX(1, dt_collection_get_collected_count());

    // so the total number of lines is :
    const uint32_t nblines = ceilf(nbid / (float)table->thumbs_per_row);

    // now we have a space under last images (when the first shown line is fully shown)
    const float pagesize = table->view_height / (float)table->thumb_size;
    const float maxvalue = nblines + (pagesize - floorf(pagesize));

    // the number of line before
    float lbefore = (table->offset - 1) / table->thumbs_per_row;
    if ((table->offset - 1) % table->thumbs_per_row)
        lbefore++;

    // if scrollbars are used, we can have partial row shown
    if (table->thumbs_area.y != 0)
    {
        lbefore += -table->thumbs_area.y / (float)table->thumb_size;
    }

    // if the scrollbar is currently visible and we want to hide it we
    // first ensure that with the width without the scrollbar, we won't
    // need a scrollbar
    const int bar = gtk_widget_get_allocated_width(darktable.gui->scrollbars.vscrollbar);
    if (gtk_widget_get_visible(darktable.gui->scrollbars.vscrollbar) && nblines <= table->rows - 1)
    {
        const int nw = table->view_width + bar;
        if (nblines * nw / table->thumbs_per_row >= table->view_height)
        {
            dt_view_set_scrollbar(darktable.view_manager->current_view, 0, 0, 0, 0, lbefore, 0,
                                  maxvalue + 1, pagesize);
            table->code_scrolling = FALSE;
            return TRUE;
        }
    }
    /* In filemanager, no horizontal bar, and vertical bar reference is 1 thumb.
     We make sure to show a scrollbar - and thus keep thumbs slightly smaller -
     if not showing it required to avoid an unstable state.
  */
    const float thresh = (float)(table->view_width / table->thumbs_per_row) /
                             (float)((table->view_width - bar) / table->thumbs_per_row) -
                         1.0f;
    dt_view_set_scrollbar(darktable.view_manager->current_view, 0, 0, 0, 0, lbefore, 0,
                          maxvalue - thresh, pagesize);
    table->code_scrolling = FALSE;
    return (lbefore >= maxvalue);
}

// remove all unneeded thumbnails from the list and the widget
// unneeded == completely hidden
static int _thumbs_remove_unneeded(dt_thumbtable_t *table, GList **th_invalid)
{
    if (!th_invalid)
        return 0;

    int changed = 0;
    for (const GList *l = *th_invalid; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(th->w_main)), th->w_main);
        dt_thumbnail_destroy(th);
        changed++;
    }
    g_list_free(*th_invalid);
    return changed;
}

static void _thumb_move_or_create(dt_thumbtable_t *table, GList **th_invalid, const int imgid,
                                  const int rowid, const int posx, const int posy,
                                  const gboolean top, const dt_thumbnail_selection_t sel)
{
    if (!th_invalid || *th_invalid == NULL)
    {
        // no widgets available. We need to create one
        dt_thumbnail_t *thumb = dt_thumbnail_new(
            table->thumb_size, table->thumb_size, IMG_TO_FIT, imgid, rowid, table->overlays,
            DT_THUMBNAIL_CONTAINER_LIGHTTABLE, table->show_tooltips, sel);
        if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
            thumb->single_click = TRUE;
            thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
        }
        thumb->x = posx;
        thumb->y = posy;
        if (top)
            table->list = g_list_prepend(table->list, thumb);
        else
            table->list = g_list_append(table->list, thumb);

        // we remember image margins for new thumbs (this limit flickering)
        dt_thumbnail_t *first = table->list->data;
        gtk_widget_set_margin_start(thumb->w_image_box,
                                    gtk_widget_get_margin_start(first->w_image_box));
        gtk_widget_set_margin_top(thumb->w_image_box,
                                  gtk_widget_get_margin_top(first->w_image_box));
        gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
    }
    else
    {
        // let's reuse a now unaffected widget
        dt_thumbnail_t *thumb = (*th_invalid)->data;
        thumb->imgid = imgid;
        thumb->rowid = rowid;
        thumb->x = posx;
        thumb->y = posy;
        dt_thumbnail_reload_infos(thumb);
        dt_thumbnail_surface_destroy(thumb);
        thumb->img_surf_preview = FALSE;
        gtk_layout_move(GTK_LAYOUT(table->widget), thumb->w_main, thumb->x, thumb->y);
        *th_invalid = g_list_delete_link(*th_invalid, *th_invalid);
        // insert the thumb at the right place in the table->list
        if (top)
            table->list = g_list_prepend(table->list, thumb);
        else
            table->list = g_list_append(table->list, thumb);
        // eventually update the selected state
        dt_thumbnail_set_selection(thumb, sel);
    }
}

// load all needed thumbnails in the list and the widget
// needed == that should appear in the current view (possibly not entirely)
static int _thumbs_load_needed(dt_thumbtable_t *table, GList **th_invalid, dt_thumbnail_t *first,
                               dt_thumbnail_t *last)
{
    if (!first || !last)
        return 0;

    sqlite3_stmt *stmt;
    int changed = 0;

    // we save here the last image value as they may change if the last image is invalid
    const int last_y = last->y;
    const int last_x = last->x;
    const int last_rowid = last->rowid;

    // we load image at the beginning
    if (first->rowid > 1 && ((table->mode == DT_THUMBTABLE_MODE_FILEMANAGER && first->y > 0) ||
                             (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP && first->x > 0)))
    {
        int space = first->y;
        if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
            space = first->x;

        const int nb_to_load = space / table->thumb_size + (space % table->thumb_size != 0);
        // clang-format off
    gchar *query = g_strdup_printf(
       "SELECT mi.rowid, mi.imgid, si.imgid"
       " FROM memory.collected_images AS mi"
       " LEFT JOIN main.selected_images AS si"
       "   ON mi.imgid = si.imgid"
       " WHERE mi.rowid<%d"
       " ORDER BY mi.rowid DESC LIMIT %d",
        first->rowid, nb_to_load * table->thumbs_per_row);
        // clang-format on
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        int posx = first->x;
        int posy = first->y;
        _pos_get_previous(table, &posx, &posy);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            if (posy < table->view_height) // we don't load invisible thumbs
            {
                const dt_imgid_t imgid = sqlite3_column_int(stmt, 1);
                const int rowid = sqlite3_column_int(stmt, 0);
                const gboolean selected = (imgid == sqlite3_column_int(stmt, 2));
                _thumb_move_or_create(table, th_invalid, imgid, rowid, posx, posy, TRUE, selected);
                changed++;
            }
            _pos_get_previous(table, &posx, &posy);
        }
        g_free(query);
        sqlite3_finalize(stmt);
    }

    // we load images at the end
    // if there's space under the last image, we have rows to load
    // if the last line is not full, we have already reached the end of the collection
    if ((table->mode == DT_THUMBTABLE_MODE_FILEMANAGER &&
         last_y + table->thumb_size < table->view_height &&
         last_x >= table->thumb_size * (table->thumbs_per_row - 1)) ||
        (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP &&
         last_x + table->thumb_size < table->view_width))
    {
        int space = table->view_height - (last_y + table->thumb_size);
        if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
            space = table->view_width - (last_x + table->thumb_size);
        const int nb_to_load = space / table->thumb_size + (space % table->thumb_size != 0);
        // clang-format off
    gchar *query = g_strdup_printf(
       "SELECT mi.rowid, mi.imgid, si.imgid"
       " FROM memory.collected_images AS mi"
       " LEFT JOIN main.selected_images AS si"
       "   ON mi.imgid = si.imgid"
       " WHERE rowid>%d"
       " ORDER BY rowid LIMIT %d",
        last_rowid, nb_to_load * table->thumbs_per_row);
        // clang-format on
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

        int posx = last_x;
        int posy = last_y;
        _pos_get_next(table, &posx, &posy);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            if (posy + table->thumb_size > 0) // we don't load invisible thumbs
            {
                const dt_imgid_t imgid = sqlite3_column_int(stmt, 1);
                const int rowid = sqlite3_column_int(stmt, 0);
                const gboolean selected = (imgid == sqlite3_column_int(stmt, 2));
                _thumb_move_or_create(table, th_invalid, imgid, rowid, posx, posy, FALSE, selected);
                changed++;
            }
            _pos_get_next(table, &posx, &posy);
        }
        g_free(query);
        sqlite3_finalize(stmt);
    }

    return changed;
}

// move all thumbs from the table.
// if clamp, we verify that the move is allowed (collection bounds, etc...)
static gboolean _move(dt_thumbtable_t *table, const int x, const int y, const gboolean clamp)
{
    if (!table->list)
        return FALSE;

    int posx = x;
    int posy = y;
    if (clamp)
    {
        // we check bounds to allow or not the move
        if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        {
            posx = 0; // to be sure, we don't want horizontal move
            if (posy == 0)
                return FALSE;

            dt_thumbnail_t *first = table->list->data;
            // clamp the movement to ensure we don't go before the first image or after last one
            const int max_up = ((first->rowid - 1) / table->thumbs_per_row) * table->thumb_size -
                               table->thumbs_area.y;
            posy = MIN(posy, max_up);

            // nb of line of the full collection
            const uint32_t nblines =
                ceilf(MAX(1, dt_collection_get_collected_count()) / (float)table->thumbs_per_row);
            // max first line on screen to ensure we don't go to far
            const int max_line = nblines - table->view_height / table->thumb_size;
            // limit of the movement
            const int max_down = MAX(0, max_line * table->thumb_size - max_up);
            posy = MAX(posy, -max_down);
        }
        else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
            posy = 0; // to be sure, we don't want vertical move
            if (posx == 0)
                return FALSE;

            // we stop when first or last rowid image is fully shown
            const dt_thumbnail_t *first = table->list->data;
            const dt_thumbnail_t *last = g_list_last(table->list)->data;
            const int middle = (table->view_width - table->thumb_size) / 2;
            posx = CLAMP(posx, middle - last->x, middle - first->x);
        }
    }

    if (posy == 0 && posx == 0)
        return FALSE;

    GList *th_invalid = NULL;
    // we move all current thumbs
    dt_thumbnail_t *first = NULL;
    dt_thumbnail_t *last = NULL;
    GList *l = table->list;
    while (l)
    {
        dt_thumbnail_t *th = l->data;
        if (!first)
            first = th;

        last = th;
        th->y += posy;
        th->x += posx;

        if (th->y + table->thumb_size <= 0 || th->y > table->view_height ||
            (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP &&
             (th->x + table->thumb_size <= 0 || th->x > table->view_width)))
        {
            th_invalid = g_list_prepend(th_invalid, th);
            GList *ll = l;
            l = g_list_next(l);
            table->list = g_list_delete_link(table->list, ll);

        }
        else
        {
            gtk_layout_move(GTK_LAYOUT(table->widget), th->w_main, th->x, th->y);
            l = g_list_next(l);
        }
    }

    // we update the thumbs_area
    const int old_areay = table->thumbs_area.y;
    table->thumbs_area.x += posx;
    table->thumbs_area.y += posy;

    // we load all needed thumbs
    int changed = _thumbs_load_needed(table, &th_invalid, first, last);
    // we remove the images not visible on screen
    changed += _thumbs_remove_unneeded(table, &th_invalid);

    // if there has been changed, we recompute thumbs area
    if (changed > 0)
        _pos_compute_area(table);

    // we update the offset
    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    {
        // we need to take account of the previous area move if needed
        table->offset =
            MAX(1, table->offset - (ceilf((posy + old_areay) / (float)table->thumb_size) *
                                    table->thumbs_per_row));
        table->offset_imgid = _thumb_get_imgid(table->offset);
    }
    else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
        table->offset = MAX(1, table->offset - posx / table->thumb_size);
        table->offset_imgid = _thumb_get_imgid(table->offset);
    }
    // and we store it
    dt_conf_set_int("plugins/lighttable/collect/history_pos0", table->offset);

    // update scrollbars
    _thumbtable_update_scrollbars(table);

    return TRUE;
}

static dt_thumbnail_t *_thumbtable_get_thumb(dt_thumbtable_t *table, const dt_imgid_t imgid)
{
    if (!dt_is_valid_imgid(imgid))
        return NULL;

    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        if (th->imgid == imgid)
            return th;
    }
    return NULL;
}

// Change the target thumbnail size while keeping the image under the pointer in place.
static void _filemanager_zoom(dt_thumbtable_t *table, const int old_size, const int new_size)
{
    // nothing to do if thumbtable is empty
    if (!table->list)
        return;

    // we are looking for the image to zoom around
    int x = 0;
    int y = 0;
    dt_thumbnail_t *thumb = NULL;
    if (table->mouse_inside)
    {
        // if the mouse is inside the table, let's use its position
        gdk_window_get_origin(gtk_widget_get_window(table->widget), &x, &y);
        x = table->last_x - x;
        y = table->last_y - y;
        thumb = _thumb_get_at_pos(table, x, y);
    }

    if (!thumb)
    {
        // otherwise we use the classic retrieving method
        const dt_imgid_t id = dt_act_on_get_main_image();
        thumb = _thumbtable_get_thumb(table, id);
        if (thumb)
        {
            // and we take the center of the thumb
            x = thumb->x + thumb->width / 2;
            y = thumb->y + thumb->height / 2;
        }
        else
        {
            // still no thumb, try to use the one at screen center
            x = table->view_width / 2;
            y = table->view_height / 2;
            thumb = _thumb_get_at_pos(table, x, y);
            if (!thumb)
            {
                // and lastly, take the first at screen
                // chained dereference is dangerous, but there was a check above in the code
                thumb = (dt_thumbnail_t *)table->list->data;
                x = thumb->x + thumb->width / 2;
                y = thumb->y + thumb->height / 2;
            }
        }
    }

    // how many images will be displayed before the current position ?
    const int new_columns = _grid_columns_for_width(table->view_width, new_size);
    const int new_cell_size = MAX(1, table->view_width / new_columns);
    const int new_pos = y / new_cell_size * new_columns + x / new_cell_size;

    dt_thumbtable_set_offset(table, thumb->rowid - new_pos, FALSE);

    dt_view_lighttable_set_zoom(darktable.view_manager, new_size);
    gtk_widget_queue_draw(table->widget);
    (void)old_size;
}

void dt_thumbtable_zoom_changed(dt_thumbtable_t *table, const int oldzoom, const int newzoom)
{
    if (oldzoom == newzoom)
        return;

    if (!table->list)
        return;

    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        _filemanager_zoom(table, oldzoom, newzoom);
}

static gboolean _event_scroll_compressed(gpointer user_data)
{
    if (!user_data)
        return FALSE;

    dt_thumbtable_t *table = user_data;

    // Thumbtable is empty, nothing to scroll
    if (table->thumb_size == 0)
        return FALSE;

    if (table->scroll_value != 0)
    {
        float delta = table->scroll_value;

        // starting from here, all further scroll event will count for the next round
        table->scroll_value = 0;

        // For filemanager and filmstrip, scrolled = move.
        // For filemanager we ensure to fallback to show full row (can be
        // half shown if scrollbar used)
        int move = table->thumb_size * delta;

        // for fractional scrolling, scroll by a number of pixels proportionate to
        // the delta (which is a float value for most touch pads and some mice)
        if (dt_conf_get_bool("thumbtable_fractional_scrolling"))
        {
            // scale scroll increment for an appropriate scroll speed
            delta *= 50;
            _move(table, 0, -delta, TRUE);
        }
        // for clicky scrolling, scroll one row of thumbnails per scroll delta
        // (which is collected into an integer value in this case)
        else
        {
            // if the top thumb row is only partially visible, then realign first
            const int partial_height = table->thumbs_area.y % table->thumb_size;
            if (partial_height)
            {
                if (delta < 0)
                {
                    move = partial_height;
                }
                else
                {
                    move = table->thumb_size + partial_height;
                }
            }
            _move(table, 0, -move, TRUE);
        }

        // ensure the hovered image is the right one
        dt_thumbnail_t *th = _thumb_get_under_mouse(table);
        if (th)
            dt_control_set_mouse_over_id(th->imgid);
    }

    // we reset the id value at the end, to ensure we don't get more
    // than 1 pending scroll
    table->scroll_timeout_id = 0;
    return FALSE;
}

static gboolean _event_scroll(GtkWidget *widget, GdkEvent *event, dt_thumbtable_t *table)
{
    GdkEventScroll *e = (GdkEventScroll *)event;
    int delta_x, delta_y;

    // file manager can either scroll fractionally and smoothly for precision
    // touch pads, or in one-thumbnail increments for clicky scroll wheels,
    // except while control is held, as that indicates zooming
    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER &&
        !dt_modifier_is(e->state, GDK_CONTROL_MASK))
    {
        gdouble deltaf_x, deltaf_y;
        gboolean did_scroll;
        if (dt_conf_get_bool("thumbtable_fractional_scrolling"))
        {
            did_scroll = dt_gui_get_scroll_deltas(e, &deltaf_x, &deltaf_y);
        }
        else
        {
            did_scroll = dt_gui_get_scroll_unit_deltas(e, &delta_x, &delta_y);
            deltaf_y = (float)delta_y;
        }
        if (did_scroll)
        {
            // in order to process "big" scroll in one time, we use a
            // timeout to postpone a little scrolling
            if (table->scroll_timeout_id == 0)
            {
                table->scroll_timeout_id = g_timeout_add(10, _event_scroll_compressed, table);
            }
            table->scroll_value += deltaf_y;
        }
        // we stop here to avoid scrolledwindow to move
        return TRUE;
    }

    // Filmstrip and Ctrl+wheel use clicky scroll.
    if (dt_gui_get_scroll_unit_deltas(e, &delta_x, &delta_y))
    {
        if (dt_modifier_is(e->state, GDK_CONTROL_MASK))
        {
            if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
            {
                const int sx = CLAMP(
                    table->view_width /
                        ((table->view_width / table->thumb_size / 2 + (delta_x + delta_y)) * 2 + 1),
                    dt_conf_get_int("min_panel_height"), dt_conf_get_int("max_panel_height"));
                dt_ui_panel_set_size(darktable.gui->ui, DT_UI_PANEL_BOTTOM, sx);
            }
            else
            {
                const int old = dt_view_lighttable_get_zoom(darktable.view_manager);
                const int new = CLAMP(old - delta_y * DT_LIGHTTABLE_GRID_THUMBNAIL_SIZE_STEP,
                                      DT_LIGHTTABLE_GRID_MIN_THUMBNAIL_SIZE,
                                      DT_LIGHTTABLE_GRID_MAX_THUMBNAIL_SIZE);
                dt_thumbtable_zoom_changed(table, old, new);
            }
        }
        else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
            _move(table,
                  -(delta_x + delta_y) * (dt_modifier_is(e->state, GDK_SHIFT_MASK) ?
                                              table->view_width - table->thumb_size :
                                              table->thumb_size),
                  0, TRUE);

            // ensure the hovered image is the right one
            dt_thumbnail_t *th = _thumb_get_under_mouse(table);
            if (th)
                dt_control_set_mouse_over_id(th->imgid);
        }
    }
    // we stop here to avoid scrolledwindow to move
    return TRUE;
}

static gboolean _event_draw(GtkWidget *widget, cairo_t *cr, dt_thumbtable_t *table)
{
    if (!GTK_IS_CONTAINER(gtk_widget_get_parent(widget)))
        return TRUE;

    // we render the background (can be visible if before first image / after last image)
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_render_background(context, cr, 0, 0, gtk_widget_get_allocated_width(widget),
                          gtk_widget_get_allocated_height(widget));

    if (!darktable.collection || (dt_collection_get_count(darktable.collection) == 0))
        return TRUE;

    dt_thumbtable_full_redraw(table, FALSE);
    return FALSE; // let's propagate this event
}

static gboolean _event_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                    dt_thumbtable_t *table)
{
    // if the leaving cause is the hide of the widget, no mouseover change
    if (!gtk_widget_is_visible(widget))
    {
        table->mouse_inside = FALSE;
        return FALSE;
    }

    // if we leave thumbtable in favour of an inferior (a thumbnail)
    // it's not a real leave !  same if this is not a mouse move action
    // (shortcut that activate a button for example)
    if (event->detail == GDK_NOTIFY_INFERIOR || event->mode == GDK_CROSSING_GTK_GRAB ||
        event->mode == GDK_CROSSING_GRAB)
        return FALSE;

    table->mouse_inside = FALSE;
    dt_control_set_mouse_over_id(NO_IMGID);
    return TRUE;
}

static gboolean _event_enter_notify(GtkWidget *widget, GdkEventCrossing *event,
                                    dt_thumbtable_t *table)
{
    dt_set_backthumb_time(0.0);

    // we only handle the case where we enter thumbtable from an inferior (a thumbnail)
    // this is when the mouse enter an "empty" area of thumbtable
    if (event->detail != GDK_NOTIFY_INFERIOR)
        return FALSE;

    dt_control_set_mouse_over_id(NO_IMGID);
    return TRUE;
}

static gboolean _do_select_single(gpointer user_data)
{
    dt_thumbtable_t *table = user_data;

    // always keep the edited picture selected
    dt_selection_clear(darktable.selection);
    dt_selection_select(darktable.selection, darktable.develop->image_storage.id);
    dt_selection_select(darktable.selection, table->to_selid);
    table->sel_single_cb = 0;

    return FALSE;
}

static void _sync_lighttable_grid_from_filmstrip(dt_thumbtable_t *table, const dt_imgid_t imgid)
{
    if (table != dt_ui_lighttable_filmstrip(darktable.gui->ui) ||
        dt_view_get_current() != DT_VIEW_LIGHTTABLE ||
        dt_view_lighttable_get_layout(darktable.view_manager) != DT_LIGHTTABLE_LAYOUT_FILEMANAGER ||
        dt_view_lighttable_preview_state(darktable.view_manager))
        return;

    dt_thumbtable_t *grid = dt_ui_thumbtable(darktable.gui->ui);
    if (!dt_thumbtable_ensure_imgid_visibility(grid, imgid))
        dt_thumbtable_set_offset_image(grid, imgid, TRUE);
}

static gboolean _event_button_press(GtkWidget *widget, GdkEventButton *event,
                                    dt_thumbtable_t *table)
{
    dt_set_backthumb_time(0.0);

    const dt_imgid_t id = dt_control_get_mouse_over_id();

    if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_SECONDARY)
        return dt_gui_context_menu_show_image(widget, id);

    if (dt_is_valid_imgid(id) && event->button == GDK_BUTTON_PRIMARY)
    {
        //  double-click
        if (event->type == GDK_2BUTTON_PRESS)
        {
            switch (table->mode)
            {
            case DT_THUMBTABLE_MODE_FILEMANAGER:
                dt_selection_select_single(darktable.selection, id);
                DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
                dt_view_lighttable_set_preview_state(
                    darktable.view_manager, TRUE, TRUE, FALSE,
                    DT_LIGHTTABLE_CULLING_RESTRICTION_AUTO);
                dt_view_lighttable_change_offset(darktable.view_manager, TRUE, id);
                break;

            case DT_THUMBTABLE_MODE_FILMSTRIP:
                if (dt_view_get_current() == DT_VIEW_DARKROOM)
                {
                    if (table->sel_single_cb != 0)
                    {
                        g_source_remove(table->sel_single_cb);
                        table->sel_single_cb = 0;
                    }
                    // disable next BUTTON_RELEASE event (see _event_motion_release)
                    table->to_selid = -1;
                    // unselect currently edited picture, select new one
                    dt_selection_deselect(darktable.selection, darktable.develop->image_storage.id);
                    dt_selection_select(darktable.selection, id);
                    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
                    return FALSE;
                }
            default:
                break;
            }
        }

        if (event->type == GDK_BUTTON_PRESS && table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
            return FALSE;
    }

    if (event->button == GDK_BUTTON_PRIMARY && event->type == GDK_BUTTON_PRESS)
    {
        // make sure any edition field loses the focus
        gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
    }

    if (!dt_is_valid_imgid(id) && event->button == GDK_BUTTON_PRIMARY &&
        event->type == GDK_BUTTON_PRESS)
    {
        const dt_view_type_flags_t cv = dt_view_get_current();

        // we click in an empty area, let's deselect all images
        dt_selection_clear(darktable.selection);

        // but we still want the currently edited image to be selected when
        // in darkroom.
        if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP && cv == DT_VIEW_DARKROOM)
        {
            dt_selection_select(darktable.selection, darktable.develop->image_storage.id);
        }

        return TRUE;
    }

    return TRUE;
}

static gboolean _event_key_press(GtkWidget *widget, GdkEventKey *event, dt_thumbtable_t *table)
{
    (void)table;
    if (event->keyval != GDK_KEY_Menu &&
        (event->keyval != GDK_KEY_F10 || !(event->state & GDK_SHIFT_MASK)))
        return FALSE;

    GList *selection = dt_selection_get_list(darktable.selection, FALSE, FALSE);
    const dt_imgid_t image = selection ? GPOINTER_TO_INT(selection->data) :
                                         dt_control_get_mouse_over_id();
    g_list_free(selection);
    return dt_gui_context_menu_show_image(widget, image);
}

static gboolean _event_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                     dt_thumbtable_t *table)
{
    dt_set_backthumb_time(0.0);

    table->mouse_inside = TRUE;

    table->last_x = ceil(event->x_root);
    table->last_y = ceil(event->y_root);
    return FALSE;
}

static gboolean _event_button_release(GtkWidget *widget, GdkEventButton *event,
                                      dt_thumbtable_t *table)
{
    // we select only in LIGHTTABLE and DARKROOM mode
    const dt_view_type_flags_t cv = dt_view_get_current();

    if (cv != DT_VIEW_DARKROOM && cv != DT_VIEW_LIGHTTABLE)
        return FALSE;

    dt_set_backthumb_time(0.0);
    const dt_imgid_t id = dt_control_get_mouse_over_id();

    if (dt_is_valid_imgid(id) && event->button == GDK_BUTTON_PRIMARY &&
        event->type == GDK_BUTTON_RELEASE)
    {
        gboolean selection_changed = FALSE;
        if (dt_modifier_is(event->state, GDK_CONTROL_MASK) ||
            dt_modifier_is(event->state, GDK_MOD2_MASK)) // CMD key on macOS
        {
            dt_selection_toggle(darktable.selection, id);
            selection_changed = TRUE;
        }
        else if (dt_modifier_is(event->state, GDK_SHIFT_MASK))
        {
            dt_selection_select_range(darktable.selection, id);
            selection_changed = TRUE;
        }
        else
        {
            if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP && cv == DT_VIEW_DARKROOM)
            {
                // if there is more than one selected image then we have at least
                // one picture selected not counting the currently edited one.
                // delay the single selection to ensure that if we double-click we
                // do not unselect all the pictures.
                if (table->sel_single_cb == 0)
                {
                    // button released event must be skip
                    if (table->to_selid == -1)
                    {
                        table->to_selid = NO_IMGID;
                    }
                    else
                    {
                        GtkSettings *settings = gtk_widget_get_settings(GTK_WIDGET(widget));
                        guint double_click_time = 400;

                        if (settings)
                        {
                            g_object_get(settings, "gtk-double-click-time", &double_click_time,
                                         NULL);
                        }

                        table->to_selid = id;
                        table->sel_single_cb =
                            g_timeout_add(double_click_time, _do_select_single, table);
                    }
                }
            }
            else
            {
                dt_selection_select_single(darktable.selection, id);
                DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
                selection_changed = TRUE;
            }
        }

        if (selection_changed)
            _sync_lighttable_grid_from_filmstrip(table, id);
    }

    return TRUE;
}

// set scrollbars visibility
static void _thumbtable_restore_scrollbars(dt_thumbtable_t *table)
{
    switch (table->mode)
    {
    case DT_THUMBTABLE_MODE_FILMSTRIP:
        table->scrollbars = dt_conf_get_bool("darkroom/ui/scrollbars");
        break;

    case DT_THUMBTABLE_MODE_FILEMANAGER:
        table->scrollbars = dt_conf_get_bool("lighttable/ui/scrollbars");
        break;

    default:
        table->scrollbars = FALSE;
        break;
    }

    dt_ui_scrollbars_show(darktable.gui->ui, table->scrollbars);
}

// propose to discard cache in case of thumb generation setting change
static void _thumbs_ask_for_discard(dt_thumbtable_t *table)
{
    // we get "new values"
    const char *hq = dt_conf_get_string_const("plugins/lighttable/thumbnail_hq_min_level");
    dt_mipmap_size_t hql = dt_mipmap_cache_get_min_mip_from_pref(hq);
    const char *embedded = dt_conf_get_string_const("plugins/lighttable/thumbnail_raw_min_level");

    dt_mipmap_size_t embeddedl = dt_mipmap_cache_get_min_mip_from_pref(embedded);

    int min_level = DT_MIPMAP_LDR_MAX;
    int max_level = DT_MIPMAP_0;
    if (hql != table->pref_hq)
    {
        min_level = MIN(table->pref_hq, hql);
        max_level = MAX(table->pref_hq, hql);
    }
    if (embeddedl != table->pref_embedded)
    {
        min_level = MIN(min_level, MIN(table->pref_embedded, embeddedl));
        max_level = MAX(max_level, MAX(table->pref_embedded, embeddedl));
    }

    // switching between auto/never options
    if (max_level == DT_MIPMAP_NONE && min_level == DT_MIPMAP_LDR_MAX)
    {
        // err on side of discarding too many thumbnails: a quick
        // survey of vintage raw files shows a lowest res embedded
        // JPEG of 1616x1080 (found in 2011 & 2014 Sony)
        min_level = DT_MIPMAP_4;
    }

    sqlite3_stmt *stmt = NULL;

    if (min_level < max_level)
    {
        gchar *txt = g_strdup(_("you have changed the settings related to"
                                " how thumbnails are generated.\n"));
        if (max_level >= DT_MIPMAP_LDR_MAX && min_level == DT_MIPMAP_0)
            dt_util_str_cat(&txt, _("all cached thumbnails need to be invalidated.\n\n"));
        else if (max_level >= DT_MIPMAP_LDR_MAX)
            dt_util_str_cat(
                &txt, _("cached thumbnails starting from level %d need to be invalidated.\n\n"),
                min_level);
        else if (min_level == DT_MIPMAP_0)
            dt_util_str_cat(&txt, _("cached thumbnails below level %d need to be invalidated.\n\n"),
                            max_level);
        else
            dt_util_str_cat(
                &txt, _("cached thumbnails between level %d and %d need to be invalidated.\n\n"),
                min_level, max_level);

        dt_util_str_cat(&txt, _("do you want to do that now?"));

        if (dt_gui_show_yes_no_dialog(_("cached thumbnails invalidation"), "", "%s", txt))
        {
            DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM main.images",
                                        -1, &stmt, NULL);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const dt_imgid_t imgid = sqlite3_column_int(stmt, 0);
                for (int i = max_level; i >= min_level; i--)
                {
                    dt_mipmap_cache_remove_at_size(imgid, i);
                }
            }
            sqlite3_finalize(stmt);
        }
        g_free(txt);
    }
    // in any case, we update thumbtable prefs values to new ones and update backthumbs database
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.images"
                                " SET thumb_maxmip = ?1"
                                " WHERE thumb_maxmip > ?1 ",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, min_level);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    table->pref_hq = hql;
    table->pref_embedded = embeddedl;
}

// called each time the preference change, to update specific parts
static void _dt_pref_change_callback(gpointer instance, dt_thumbtable_t *table)
{
    // in all case, we reset the act_on cache as the algorithm may have changed
    dt_act_on_reset_cache(TRUE);
    dt_act_on_reset_cache(FALSE);

    if (!table)
        return;

    dt_stop_backthumbs_crawler(FALSE);
    // adjust the act_on algo class if needed
    dt_act_on_set_class(table->widget);

    dt_get_sysresource_level();
    dt_opencl_update_settings();
    dt_configure_ppd_dpi(darktable.gui);

    _thumbs_ask_for_discard(table);

    dt_thumbtable_full_redraw(table, TRUE);

    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        if (th)
        {
            dt_thumbnail_reload_infos(th);
            dt_thumbnail_resize(th, th->width, th->height, TRUE, IMG_TO_FIT);
        }
    }
    dt_start_backthumbs_crawler();
}

static void _dt_profile_change_callback(gpointer instance, const int type, dt_thumbtable_t *table)
{
    if (!table)
        return;

    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        dt_thumbnail_image_refresh(th);
    }
}

static void _dt_metadata_change_callback(gpointer instance, const int type, dt_thumbtable_t *table)
{
    if (!table)
        return;

    GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);

    //  Update thumbnails information as they are possibly using
    //  metadata $(Xmp.??.???) into their tooltips.
    for (const GList *l = imgs; l; l = g_list_next(l))
    {
        const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
        dt_thumbnail_t *thumb = _thumbtable_get_thumb(table, imgid);
        if (thumb)
            dt_thumbnail_reload_infos(thumb);
    }

    g_list_free(imgs);
}

static void _dt_tag_change_callback(gpointer instance, dt_thumbtable_t *table)
{
    _dt_metadata_change_callback(instance, 0, table);
}

static gboolean _filmstrip_ensure_imgid_visibility(dt_thumbtable_t *table, const dt_imgid_t imgid)
{
    if (!dt_is_valid_imgid(imgid))
        return FALSE;
    if (!table->list)
        return FALSE;

    // Check if the thumb is already in the loaded list
    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        const dt_thumbnail_t *th = l->data;
        if (th->imgid == imgid)
        {
            // Already fully visible - nothing to do
            if (th->x >= 0 && th->x + table->thumb_size <= table->view_width)
                return TRUE;
            // Scroll the minimum amount to bring it fully into view
            int x_move = 0;
            if (th->x < 0)
                x_move = -th->x;
            else
                x_move = table->view_width - th->x - table->thumb_size;
            return _move(table, x_move, 0, FALSE);
        }
    }

    // Thumb not yet loaded - compute its expected position relative to the
    // loaded range and scroll exactly enough to bring it into view
    const dt_thumbnail_t *first = table->list->data;
    const dt_thumbnail_t *last = g_list_last(table->list)->data;
    const int rowid = _thumb_get_rowid(imgid);
    if (rowid < 1)
        return FALSE;

    int x_move = 0;
    if (rowid < first->rowid)
    {
        // Target is left of the loaded range; bring its left edge to x=0
        const int target_x = first->x - (first->rowid - rowid) * table->thumb_size;
        x_move = -target_x;
    }
    else if (rowid > last->rowid)
    {
        // Target is right of the loaded range; bring its right edge to view_width
        const int target_x = last->x + (rowid - last->rowid) * table->thumb_size;
        x_move = table->view_width - target_x - table->thumb_size;
    }
    else
        return FALSE;

    return _move(table, x_move, 0, FALSE);
}

// this is called each time the list of active images change
static void _dt_active_images_callback(gpointer instance, dt_thumbtable_t *table)
{
    // we only ensure here that the active image is the offset one
    // everything else (css, etc...) is handled in dt_thumbnail_t
    if (!table)
        return;

    if (!darktable.view_manager->active_images)
        return;
    const int activeid = GPOINTER_TO_INT(darktable.view_manager->active_images->data);
    if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP &&
        !dt_conf_get_bool("filmstrip/ui/auto_scroll"))
    {
        _filmstrip_ensure_imgid_visibility(table, activeid);
        return;
    }
    dt_thumbtable_set_offset_image(table, activeid, TRUE);
}

// this is called each time mouse_over id change
static void _dt_mouse_over_image_callback(gpointer instance, dt_thumbtable_t *table)
{
    if (!table)
        return;

    const dt_imgid_t imgid = dt_control_get_mouse_over_id();

    // Lighttable owns two independent thumbnail tables. The global hover id is
    // still needed by actions and menus, but its visual state belongs only to
    // the table currently under the pointer.
    if (dt_view_get_current() == DT_VIEW_LIGHTTABLE &&
        dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_FILEMANAGER &&
        !dt_view_lighttable_preview_state(darktable.view_manager) && !table->mouse_inside &&
        dt_is_valid_imgid(imgid))
        return;

    dt_imgid_t groupid = NO_IMGID;
    // we crawl over all images to find the right one
    for (const GList *l = table->list; l; l = g_list_next(l))
    {
        dt_thumbnail_t *th = l->data;
        // if needed, the change mouseover value of the thumb
        if (th->mouse_over != (th->imgid == imgid))
            dt_thumbnail_set_mouseover(th, (th->imgid == imgid));
        // now the grouping stuff
        if (th->imgid == imgid && th->is_grouped)
            groupid = th->groupid;
        if (th->group_borders)
        {
            // to be sure we don't have any borders remaining
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_NONE);
        }
    }

    // we recrawl over all image for groups borders this is somewhat
    // complex as we want to draw borders around the group and not
    // around each image of the group
    if (dt_is_valid_imgid(groupid))
    {
        int pos = 0;
        const int table_len = g_list_length(table->list);

        for (GList *l = table->list; l; l = g_list_next(l))
        {
            dt_thumbnail_t *th = l->data;
            dt_thumbnail_border_t old_borders = th->group_borders;
            if (th->groupid == groupid)
            {
                gboolean b = TRUE;
                if (table->mode != DT_THUMBTABLE_MODE_FILMSTRIP)
                {
                    // left border
                    if (pos != 0 && th->x != table->thumbs_area.x)
                    {
                        dt_thumbnail_t *th1 = g_list_previous(l)->data;
                        if (th1->groupid == groupid)
                            b = FALSE;
                    }
                    if (b)
                    {
                        dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_LEFT);
                    }
                    // right border
                    b = TRUE;
                    if (pos < table_len - 1 && (th->x + th->width * 1.5) < table->thumbs_area.width)
                    {
                        dt_thumbnail_t *th1 = g_list_next(l)->data;
                        if (th1->groupid == groupid)
                            b = FALSE;
                    }
                    if (b)
                    {
                        dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_RIGHT);
                    }
                }
                else
                {
                    // in filmstrip, top and left borders are always here (no
                    // images above or below)
                    dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_TOP);
                    dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_BOTTOM);
                }

                // top border
                b = TRUE;
                if (pos - table->thumbs_per_row >= 0)
                {
                    dt_thumbnail_t *th1 = g_list_nth_prev(l, table->thumbs_per_row)->data;
                    if (th1->groupid == groupid)
                        b = FALSE;
                }
                if (b)
                {
                    if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
                        dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_LEFT);
                    else
                        dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_TOP);
                }
                // bottom border
                b = TRUE;
                if (pos + table->thumbs_per_row < table_len)
                {
                    dt_thumbnail_t *th1 = g_list_nth_data(l, table->thumbs_per_row);
                    if (th1->groupid == groupid)
                        b = FALSE;
                }
                if (b)
                {
                    if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
                        dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_RIGHT);
                    else
                        dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_BOTTOM);
                }
            }
            if (th->group_borders != old_borders)
                gtk_widget_queue_draw(th->w_back);
            pos++;
        }
    }
}

// this is called each time collected images change
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change,
                                            dt_collection_properties_t changed_property,
                                            gpointer imgs, const dt_imgid_t next,
                                            dt_thumbtable_t *table)
{
    if (!table)
        return;

    dt_collection_history_save();

    if (query_change == DT_COLLECTION_CHANGE_RELOAD)
    {
        dt_imgid_t old_hover = dt_control_get_mouse_over_id();
        /** Here's how it works
     *
     *          list of change|   | x | x | x | x |
     *  offset inside the list| ? |   | x | x | x |
     * offset rowid as changed| ? | ? |   | x | x |
     *     next imgid is valid| ? | ? | ? |   | x |
     *                        |   |   |   |   |   |
     *                        | S | S | S | S | N |
     * S = same imgid as offset ; N = next imgid as offset
     **/

        // in filmstrip mode, let's first ensure the offset is the right
        // one. Otherwise we move to it
        int old_offset = -1;
        if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP && darktable.view_manager->active_images)
        {
            const int tmpoff = GPOINTER_TO_INT(darktable.view_manager->active_images->data);
            if (tmpoff != table->offset_imgid)
            {
                old_offset = table->offset_imgid;
                table->offset = _thumb_get_rowid(tmpoff);
                table->offset_imgid = tmpoff;
                dt_thumbtable_full_redraw(table, TRUE);
            }
        }
        dt_imgid_t newid = table->offset_imgid;
        if (newid <= 0 && table->offset > 0)
            newid = _thumb_get_imgid(table->offset);

        // is the current offset imgid in the changed list
        gboolean in_list = FALSE;
        for (const GList *l = imgs; l; l = g_list_next(l))
        {
            if (table->offset_imgid == GPOINTER_TO_INT(l->data))
            {
                in_list = TRUE;
                break;
            }
        }

        if (in_list)
        {
            if (dt_is_valid_imgid(next) && _thumb_get_rowid(table->offset_imgid) != table->offset)
            {
                // if offset has changed, that means the offset img has
                // moved. So we use the next untouched image as offset but we
                // have to ensure next is in the selection if we navigate
                // inside sel.
                newid = next;
                if (table->navigate_inside_selection)
                {
                    sqlite3_stmt *stmt;
                    // clang-format off
          gchar *query = g_strdup_printf(
              "SELECT m.imgid"
              " FROM memory.collected_images AS m, main.selected_images AS s"
              " WHERE m.imgid=s.imgid"
              "   AND m.rowid>=(SELECT rowid FROM memory.collected_images WHERE imgid=%d)"
              " ORDER BY m.rowid LIMIT 1",
              next);
                    // clang-format on
                    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt,
                                                NULL);
                    if (sqlite3_step(stmt) == SQLITE_ROW)
                    {
                        newid = sqlite3_column_int(stmt, 0);
                    }
                    else
                    {
                        // no select image after, search before
                        g_free(query);
                        sqlite3_finalize(stmt);
                        // clang-format off
            query = g_strdup_printf(
                "SELECT m.imgid"
                " FROM memory.collected_images AS m, main.selected_images AS s"
                " WHERE m.imgid=s.imgid"
                "   AND m.rowid<(SELECT rowid FROM memory.collected_images WHERE imgid=%d)"
                " ORDER BY m.rowid DESC LIMIT 1",
                next);
                        // clang-format on
                        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt,
                                                    NULL);
                        if (sqlite3_step(stmt) == SQLITE_ROW)
                        {
                            newid = sqlite3_column_int(stmt, 0);
                        }
                    }
                    g_free(query);
                    sqlite3_finalize(stmt);
                }
            }
        }

        // get the new rowid of the new offset image
        int nrow = _thumb_get_rowid(newid);

        // if we don't have a valid rowid that means the image with newid
        // doesn't exist in the new memory.collected_images as we still
        // have the "old" list of images available in table->list, let's
        // found the next valid image inside
        GList *actual = NULL;
        if (nrow <= 0)
        {
            // we retrieve the entry with id == newid
            for (GList *l = table->list; l; l = g_list_next(l))
            {
                dt_thumbnail_t *thumb = l->data;
                if (thumb->imgid == newid)
                {
                    actual = l;
                    break;
                }
            }
            // we search the first valid item after
            for (const GList *l = g_list_next(actual); l; l = g_list_next(l))
            {
                dt_thumbnail_t *thumb = l->data;
                nrow = _thumb_get_rowid(thumb->imgid);
                if (nrow > 0)
                {
                    newid = thumb->imgid;
                    break;
                }
            }
        }
        // last chance if still not valid, we search the first previous valid image
        if (nrow <= 0 && actual)
        {
            // we search the first valid item before
            for (const GList *l = g_list_previous(actual); l; l = g_list_previous(l))
            {
                dt_thumbnail_t *thumb = l->data;
                nrow = _thumb_get_rowid(thumb->imgid);
                if (nrow > 0)
                {
                    newid = thumb->imgid;
                    break;
                }
            }
        }

        const gboolean offset_changed = (MAX(1, nrow) != table->offset);
        if (nrow >= 1)
            table->offset_imgid = newid;
        else
            table->offset_imgid = _thumb_get_imgid(1);
        table->offset = MAX(1, nrow);
        if (offset_changed)
            dt_conf_set_int("plugins/lighttable/collect/history_pos0", table->offset);

        dt_thumbtable_full_redraw(table, TRUE);

        if (offset_changed)
            dt_view_lighttable_change_offset(darktable.view_manager, FALSE, table->offset_imgid);
        else
        {
            // if we are in culling or preview mode, ensure to refresh
            // active images.
            dt_view_lighttable_culling_preview_refresh(darktable.view_manager);
        }

        // if needed, we restore back the position of the filmstrip
        if (old_offset > 0 && old_offset != table->offset)
        {
            const int tmpoff = _thumb_get_rowid(old_offset);
            if (tmpoff > 0)
            {
                table->offset = tmpoff;
                table->offset_imgid = old_offset;
                dt_thumbtable_full_redraw(table, TRUE);
            }
        }

        // if the previous hovered image isn't here anymore, try to hover
        // "next" image.
        if (dt_is_valid_imgid(old_hover) && dt_is_valid_imgid(next))
        {
            // except for darkroom when mouse is not in filmstrip (the
            // active image primes).
            if (table->mouse_inside || dt_view_get_current() != DT_VIEW_DARKROOM)
            {
                in_list = FALSE;
                gboolean in_list_next = FALSE;
                for (const GList *l = table->list; l; l = g_list_next(l))
                {
                    dt_thumbnail_t *thumb = l->data;
                    if (thumb->imgid == old_hover)
                        in_list = TRUE;
                    if (thumb->imgid == next)
                        in_list_next = TRUE;
                }
                if (!in_list && in_list_next)
                    dt_control_set_mouse_over_id(next);
            }
        }
        dt_control_queue_redraw_center();
    }
    else
    {
        // otherwise we reset the offset to the wanted position or the beginning
        const int nextpos = MAX(dt_conf_get_int("plugins/lighttable/collect/history_next_pos"), 1);
        table->offset = nextpos;
        table->offset_imgid = _thumb_get_imgid(table->offset);
        dt_conf_set_int("plugins/lighttable/collect/history_pos0", nextpos);
        dt_conf_set_int("plugins/lighttable/collect/history_next_pos", 0);
        dt_thumbtable_full_redraw(table, TRUE);
        dt_view_lighttable_change_offset(darktable.view_manager, TRUE, table->offset_imgid);
    }
}

static void _event_dnd_get(GtkWidget *widget, GdkDragContext *context,
                           GtkSelectionData *selection_data, const guint target_type,
                           const guint time, dt_thumbtable_t *table)
{
    if (!table->drag_list)
        return;
    g_assert(selection_data != NULL);

    switch (target_type)
    {
    case DND_TARGET_IMGID:
    {
        const int imgs_nb = g_list_length(table->drag_list);
        if (imgs_nb)
        {
            dt_imgid_t *imgs = calloc(imgs_nb, sizeof(dt_imgid_t));
            if (!imgs)
            {
                dt_print(DT_DEBUG_ALWAYS, "[thumbtable] out of memory preparing drop target");
                break;
            }
            GList *l = table->drag_list;

            int idx = 0;
            // make sure that imgs[0] is the last selected imgid, that is the
            // one clicked when starting the d&d.
            if (dt_is_valid_imgid(darktable.control->last_clicked_filmstrip_id))
            {
                imgs[idx] = darktable.control->last_clicked_filmstrip_id;
                idx++;
            }

            while (l)
            {
                const dt_imgid_t id = GPOINTER_TO_INT(l->data);
                if (id != imgs[0])
                {
                    imgs[idx] = id;
                    idx++;
                    if (idx >= imgs_nb)
                        break;
                }
                l = g_list_next(l);
            }
            gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                                   _DWORD, (guchar *)imgs, imgs_nb * sizeof(dt_imgid_t));
        }
        break;
    }
    default: // return the location of the file as a last resort
    case DND_TARGET_URI:
    {
        GList *l = table->drag_list;
        if (g_list_is_singleton(l))
        {
            gchar pathname[PATH_MAX] = {0};
            gboolean from_cache = TRUE;
            const dt_imgid_t id = GPOINTER_TO_INT(l->data);
            dt_image_full_path(id, pathname, sizeof(pathname), &from_cache);
            gchar *uri = g_strdup_printf("file://%s", pathname);
            // TODO: should we add the host?
            gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                                   _BYTE, (guchar *)uri, strlen(uri));
            g_free(uri);
        }
        else
        {
            GList *images = NULL;
            for (; l; l = g_list_next(l))
            {
                const dt_imgid_t id = GPOINTER_TO_INT(l->data);
                gchar pathname[PATH_MAX] = {0};
                gboolean from_cache = TRUE;
                dt_image_full_path(id, pathname, sizeof(pathname), &from_cache);
                gchar *uri = g_strdup_printf("file://%s", pathname);
                // TODO: should we add the host?
                images = g_list_prepend(images, uri);
            }
            images = g_list_reverse(images);
            // list was built in reverse order, so un-reverse it
            gchar *uri_list = dt_util_glist_to_str("\r\n", images);
            g_list_free_full(images, g_free);
            gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                                   _BYTE, (guchar *)uri_list, strlen(uri_list));
            g_free(uri_list);
        }
        break;
    }
    }
}

static void _event_dnd_begin(GtkWidget *widget, GdkDragContext *context, dt_thumbtable_t *table)
{
    const int ts = DT_PIXEL_APPLY_DPI(128);

    darktable.control->last_clicked_filmstrip_id = dt_control_get_mouse_over_id();
    table->drag_list = dt_act_on_get_images(FALSE, TRUE, TRUE);

    {
        // if we are dragging a single image -> use the thumbnail of that image
        // otherwise use the generic d&d icon
        // TODO: have something pretty in the 2nd case, too.
        if (g_list_is_singleton(table->drag_list))
        {
            const int id = GPOINTER_TO_INT(table->drag_list->data);
            dt_mipmap_buffer_t buf;
            dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(ts, ts);
            dt_mipmap_cache_get(&buf, id, mip, DT_MIPMAP_BLOCKING, 'r');

            if (buf.buf)
            {
                for (size_t i = 3; i < (size_t)4 * buf.width * buf.height; i += 4)
                    buf.buf[i] = UINT8_MAX;

                int w = ts, h = ts;
                if (buf.width < buf.height)
                    w = (buf.width * ts) / buf.height; // portrait
                else
                    h = (buf.height * ts) / buf.width; // landscape

                GdkPixbuf *source =
                    gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, buf.width,
                                             buf.height, buf.width * 4, NULL, NULL);
                GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
                gtk_drag_set_icon_pixbuf(context, scaled, 0, h);

                if (source)
                    g_object_unref(source);
                if (scaled)
                    g_object_unref(scaled);
            }

            dt_mipmap_cache_release(&buf);
        }
    }
    // if we can reorder, let's update the thumbtable class accordingly
    // this will show up vertical bar for the image destination point
    if (darktable.collection->params.sorts[DT_COLLECTION_SORT_CUSTOM_ORDER])
    {
        // we set the class correctly
        dt_gui_add_class(table->widget, "dt_thumbtable_reorder");
    }
}

void dt_thumbtable_event_dnd_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                      GtkSelectionData *selection_data, guint target_type,
                                      guint time, dt_thumbtable_t *table)
{
    gboolean success = FALSE;

    if ((target_type == DND_TARGET_URI) && (selection_data != NULL) &&
        (gtk_selection_data_get_length(selection_data) >= 0))
    {
        gchar **uri_list =
            g_strsplit_set((gchar *)gtk_selection_data_get_data(selection_data), "\r\n", 0);
        if (uri_list)
        {
            gchar **image_to_load = uri_list;
            while (*image_to_load)
            {
                if (**image_to_load)
                {
                    dt_load_from_string(*image_to_load, FALSE, NULL);
                    // TODO: do we want to open the image in darkroom mode?
                    // If yes -> set to TRUE.
                }
                image_to_load++;
            }
        }
        g_strfreev(uri_list);
        success = TRUE;
    }
    else if ((target_type == DND_TARGET_IMGID) && (selection_data != NULL) &&
             (gtk_selection_data_get_length(selection_data) >= 0))
    {
        if (table->drag_list)
        {
            if (darktable.collection->params.sorts[DT_COLLECTION_SORT_CUSTOM_ORDER])
            {
                // source = dest = thumbtable => we are reordering
                // set order to "user defined" (this shouldn't trigger anything)
                const dt_imgid_t mouse_over_id = dt_control_get_mouse_over_id();
                dt_collection_move_before(mouse_over_id, table->drag_list);
                dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                           DT_COLLECTION_PROP_UNDEF, g_list_copy(table->drag_list));
                success = TRUE;
            }
        }
        else
        {
            // we don't catch anything here at the moment
        }
    }
    gtk_drag_finish(context, success, FALSE, time);
}

static void _event_dnd_end(GtkWidget *widget, GdkDragContext *context, dt_thumbtable_t *table)
{
    if (table->drag_list)
    {
        g_list_free(table->drag_list);
        table->drag_list = NULL;
    }
    // in any case, with reset the reordering class if any
    dt_gui_remove_class(table->widget, "dt_thumbtable_reorder");
}

static void _thumbtable_init_accels();

dt_thumbtable_t *dt_thumbtable_new()
{
    dt_thumbtable_t *table = calloc(1, sizeof(dt_thumbtable_t));
    table->widget = gtk_layout_new(NULL, NULL);
    dt_gui_add_help_link(table->widget, "lighttable_filemanager");

    // get thumb generation pref for reference in case of change
    const char *tx = dt_conf_get_string_const("plugins/lighttable/thumbnail_hq_min_level");
    table->pref_hq = dt_mipmap_cache_get_min_mip_from_pref(tx);
    tx = dt_conf_get_string_const("plugins/lighttable/thumbnail_raw_min_level");
    table->pref_embedded = dt_mipmap_cache_get_min_mip_from_pref(tx);

    // set css name and class
    gtk_widget_set_name(table->widget, "thumbtable-filemanager");
    dt_gui_add_class(table->widget, "dt_thumbtable");
    if (dt_conf_get_bool("lighttable/ui/expose_statuses"))
        dt_gui_add_class(table->widget, "dt_show_overlays");
    // adjust the act_on algo class if needed
    dt_act_on_set_class(table->widget);

    // overlays mode
    table->overlays = DT_THUMBNAIL_OVERLAYS_NONE;
    gchar *cl = _thumbs_get_overlays_class(table->overlays);
    dt_gui_add_class(table->widget, cl);
    g_free(cl);

    table->offset = MAX(1, dt_conf_get_int("plugins/lighttable/collect/history_pos0"));

    // set widget signals
    gtk_widget_set_events(table->widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK |
                                             GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                             GDK_STRUCTURE_MASK | GDK_ENTER_NOTIFY_MASK |
                                             GDK_LEAVE_NOTIFY_MASK);
    gtk_widget_set_app_paintable(table->widget, TRUE);
    gtk_widget_set_can_focus(table->widget, TRUE);

    // drag and drop : used for reordering, interactions with maps,
    // exporting uri to external apps, importing images in filmroll...
    gtk_drag_source_set(table->widget, GDK_BUTTON1_MASK, target_list_all, n_targets_all,
                        GDK_ACTION_MOVE);
    gtk_drag_dest_set(table->widget, GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all,
                      GDK_ACTION_MOVE);
    g_signal_connect_after(table->widget, "drag-begin", G_CALLBACK(_event_dnd_begin), table);
    g_signal_connect_after(table->widget, "drag-end", G_CALLBACK(_event_dnd_end), table);
    g_signal_connect(table->widget, "drag-data-get", G_CALLBACK(_event_dnd_get), table);
    g_signal_connect(table->widget, "drag-data-received",
                     G_CALLBACK(dt_thumbtable_event_dnd_received), table);

    g_signal_connect(G_OBJECT(table->widget), "scroll-event", G_CALLBACK(_event_scroll), table);
    g_signal_connect(G_OBJECT(table->widget), "draw", G_CALLBACK(_event_draw), table);
    g_signal_connect(G_OBJECT(table->widget), "leave-notify-event", G_CALLBACK(_event_leave_notify),
                     table);
    g_signal_connect(G_OBJECT(table->widget), "enter-notify-event", G_CALLBACK(_event_enter_notify),
                     table);
    g_signal_connect(G_OBJECT(table->widget), "button-press-event", G_CALLBACK(_event_button_press),
                     table);
    g_signal_connect(G_OBJECT(table->widget), "key-press-event", G_CALLBACK(_event_key_press),
                     table);
    g_signal_connect(G_OBJECT(table->widget), "motion-notify-event",
                     G_CALLBACK(_event_motion_notify), table);
    g_signal_connect(G_OBJECT(table->widget), "button-release-event",
                     G_CALLBACK(_event_button_release), table);

    // we register globals signals
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_COLLECTION_CHANGED, _dt_collection_changed_callback, table);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE, _dt_mouse_over_image_callback,
                              table);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_ACTIVE_IMAGES_CHANGE, _dt_active_images_callback, table);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, _dt_profile_change_callback,
                              table);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_PREFERENCES_CHANGE, _dt_pref_change_callback, table);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_METADATA_CHANGED, _dt_metadata_change_callback, table);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_TAG_CHANGED, _dt_tag_change_callback, table);
    gtk_widget_show(table->widget);

    g_object_ref(table->widget);

    // we init key accels
    _thumbtable_init_accels();

    table->sel_single_cb = 0;
    table->to_selid = NO_IMGID;
    return table;
}

void dt_thumbtable_scrollbar_changed(dt_thumbtable_t *table, const float x, const float y)
{
    if (!table->list || table->code_scrolling || !table->scrollbars)
        return;

    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    {
        // get first visible line position
        const float first_line = ((table->offset - 1) / table->thumbs_per_row) * table->thumb_size -
                                 table->thumbs_area.y;

        _move(table, 0, first_line - y * table->thumb_size, TRUE);
    }
}

// reload all thumbs from scratch.  force define if this should occurs
// in any case or just if thumbtable sizing properties have changed
void dt_thumbtable_full_redraw(dt_thumbtable_t *table, const gboolean force)
{
    if (!table)
        return;

    if (_compute_sizes(table, force))
    {
        // we update the scrollbars
        _thumbtable_update_scrollbars(table);

        const double start = dt_get_debug_wtime();
        sqlite3_stmt *stmt;
        dt_print(DT_DEBUG_LIGHTTABLE,
                 "reload thumbs from db. force=%d w=%d h=%d zoom=%d rows=%d size=%d"
                 " offset=%d centering=%d...",
                 force, table->view_width, table->view_height, table->thumbs_per_row, table->rows,
                 table->thumb_size, table->offset, table->center_offset);

        int posx = 0;
        int posy = 0;
        int offset = table->offset;

        if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        {
            // in filemanager, we need to take care of the center offset
            posx = table->center_offset;

            // ensure that the overall layout doesn't change
            // (i.e. we don't get empty spaces in the very first row)
            offset = (table->offset - 1) / table->thumbs_per_row * table->thumbs_per_row + 1;

            // ensure that we don't go up too far (we only want a space
            // <thumb_size at the bottom).
            if (table->offset != offset && offset > 1 && table->thumbs_per_row > 1)
            {
                const uint32_t nb = dt_collection_get_collected_count();
                // get how many full blank line we have at the bottom

                const int move =
                    (table->rows - 1) -
                    ((nb - (offset - 1) + table->thumbs_per_row - 1) / table->thumbs_per_row);
                if (move > 0)
                    offset = MAX(1, offset - move * table->thumbs_per_row);
            }
            table->offset = offset;
        }
        else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
            // The active image stays near the middle of the visible range, but
            // the strip itself starts at the leading edge of its container.
            offset = MAX(1, table->offset - table->rows / 2);
        }

        // we store image margin from first thumb to apply to new ones and
        // limit flickering
        int old_margin_start = 0;
        int old_margin_top = 0;
        if (table->list)
        {
            dt_thumbnail_t *first = table->list->data;
            old_margin_start = gtk_widget_get_margin_start(first->w_image_box);
            old_margin_top = gtk_widget_get_margin_top(first->w_image_box);
            // if margins > thumb size, then margins are irrelevant (thumb
            // size as just changed), better set them to 0
            if (old_margin_start >= table->thumb_size || old_margin_top >= table->thumb_size)
            {
                old_margin_start = 0;
                old_margin_top = 0;
            }
        }

        // let's create a hashtable of table->list in order to speddup
        // search in next loop.
        GHashTable *htable =
            g_hash_table_new_full(g_int_hash, g_int_equal, NULL, _list_remove_thumb);
        for (const GList *l = table->list; l; l = g_list_next(l))
        {
            dt_thumbnail_t *th = l->data;
            g_hash_table_insert(htable, &th->imgid, (gpointer)th);
        }
        g_list_free(table->list);
        table->list = NULL;

        // we add the thumbs
        int nbnew = 0;
        gchar *query = g_strdup_printf("SELECT mi.rowid, mi.imgid, si.imgid"
                                       " FROM memory.collected_images AS mi"
                                       " LEFT JOIN main.selected_images AS si"
                                       "   ON mi.imgid = si.imgid"
                                       " WHERE mi.rowid>=%d LIMIT %d",
                                       offset, table->rows * table->thumbs_per_row);

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const int nrow = sqlite3_column_int(stmt, 0);
            const int nid = sqlite3_column_int(stmt, 1);
            const gboolean selected = (nid == sqlite3_column_int(stmt, 2));

            // first, we search if the thumb is already here
            dt_thumbnail_t *thumb = (dt_thumbnail_t *)g_hash_table_lookup(htable, &nid);

            if (thumb)
            {
                g_hash_table_steal(htable, &nid);
                dt_gui_remove_class(thumb->w_main, "dt_last_active");
                thumb->rowid = nrow; // this may have changed
                // we set new position/size if needed
                if (thumb->x != posx || thumb->y != posy)
                {
                    thumb->x = posx;
                    thumb->y = posy;
                    gtk_layout_move(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
                }
                dt_thumbnail_resize(thumb, table->thumb_size, table->thumb_size, FALSE, IMG_TO_FIT);
                table->list = g_list_prepend(table->list, thumb);

                // if there's a change in selection, update the thumb
                dt_thumbnail_set_selection(thumb, selected);
            }
            else
            {
                // we create a completely new thumb
                thumb = dt_thumbnail_new(table->thumb_size, table->thumb_size, IMG_TO_FIT, nid,
                                         nrow, table->overlays, DT_THUMBNAIL_CONTAINER_LIGHTTABLE,
                                         table->show_tooltips, selected);
                if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
                {
                    thumb->single_click = TRUE;
                    thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
                }
                thumb->x = posx;
                thumb->y = posy;
                table->list = g_list_prepend(table->list, thumb);
                gtk_widget_set_margin_start(thumb->w_image_box, old_margin_start);
                gtk_widget_set_margin_top(thumb->w_image_box, old_margin_top);
                gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
                nbnew++;
            }
            _pos_get_next(table, &posx, &posy);
            // if it's the offset, we record the imgid
            if (nrow == table->offset)
                table->offset_imgid = nid;
        }

        // clean up all remaining thumbnails
        g_hash_table_destroy(htable);
        // list was built in reverse order, so un-reverse it
        table->list = g_list_reverse(table->list);

        _pos_compute_area(table);

        if (darktable.view_manager->active_images &&
            table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        {
            // this mean we arrive from filmstrip with some active images we
            // need to ensure they are visible and to mark them with some
            // css effect
            const dt_imgid_t lastid =
                GPOINTER_TO_INT(g_slist_last(darktable.view_manager->active_images)->data);
            dt_thumbtable_ensure_imgid_visibility(table, lastid);

            for (GSList *l = darktable.view_manager->active_images; l; l = g_slist_next(l))
            {
                dt_thumbnail_t *th = _thumbtable_get_thumb(table, GPOINTER_TO_INT(l->data));
                if (th)
                {
                    dt_gui_add_class(th->w_main, "dt_last_active");
                    th->active = FALSE;
                    dt_thumbnail_update_infos(th);
                }
            }
            g_slist_free(darktable.view_manager->active_images);
            darktable.view_manager->active_images = NULL;
            DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
        }

        dt_print(DT_DEBUG_LIGHTTABLE, "done in %0.04f sec %d thumbs reloaded",
                 dt_get_wtime() - start, nbnew);
        g_free(query);
        sqlite3_finalize(stmt);

        if (darktable.unmuted & DT_DEBUG_CACHE)
            dt_mipmap_cache_print();
    }
}

// change thumbtable parent widget. Typically from center screen to
// filmstrip lib.
void dt_thumbtable_set_parent(dt_thumbtable_t *table, GtkWidget *new_parent,
                              const dt_thumbtable_mode_t mode)
{
    GtkWidget *parent = gtk_widget_get_parent(table->widget);
    if (!GTK_IS_CONTAINER(new_parent))
    {
        if (parent)
        {
            // we just want to remove thumbtable from its parent
            gtk_container_remove(GTK_CONTAINER(parent), table->widget);
        }
        return;
    }

    // if table already has parent, then we remove it
    if (parent && parent != new_parent)
    {
        gtk_container_remove(GTK_CONTAINER(parent), table->widget);
    }

    // mode change
    if (table->mode != mode)
    {
        // we change the widget name
        if (mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        {
            gtk_widget_set_name(table->widget, "thumbtable-filemanager");
            dt_gui_add_help_link(table->widget, "lighttable_filemanager");
        }
        else if (mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
            gtk_widget_set_name(table->widget, "thumbtable-filmstrip");
            dt_gui_add_help_link(table->widget, "filmstrip");
        }
        // we set selection/activation properties of all thumbs
        dt_thumbnail_selection_mode_t sel_mode = DT_THUMBNAIL_SEL_MODE_NORMAL;
        gboolean single_click = FALSE;
        if (mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
            sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
            single_click = TRUE;
        }
        for (const GList *l = table->list; l; l = g_list_next(l))
        {
            dt_thumbnail_t *th = l->data;
            th->sel_mode = sel_mode;
            th->single_click = single_click;
        }

        table->mode = mode;

        // we force overlays update as the size may not change in certain cases
        _thumbs_update_overlays_mode(table);
    }

    // do we show scrollbars ?
    table->code_scrolling = TRUE;
    _thumbtable_restore_scrollbars(table);

    // we reparent the table
    if (!parent || parent != new_parent)
    {
        if (GTK_IS_OVERLAY(new_parent))
        {
            gtk_overlay_add_overlay(GTK_OVERLAY(new_parent), table->widget);
            // be sure that log msg is always placed on top
            if (new_parent == dt_ui_center_base(darktable.gui->ui))
            {
                gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                                            gtk_widget_get_parent(dt_ui_log_msg(darktable.gui->ui)),
                                            -1);
                gtk_overlay_reorder_overlay(
                    GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                    gtk_widget_get_parent(dt_ui_toast_msg(darktable.gui->ui)), -1);
            }
        }
        else
            gtk_container_add(GTK_CONTAINER(new_parent), table->widget);
    }
    table->code_scrolling = FALSE;
}

// get current offset
int dt_thumbtable_get_offset(dt_thumbtable_t *table)
{
    return table->offset;
}
// set offset and redraw if needed
gboolean dt_thumbtable_set_offset(dt_thumbtable_t *table, const int offset, const gboolean redraw)
{
    if (offset < 1 || offset == table->offset)
        return FALSE;
    table->offset = offset;
    dt_conf_set_int("plugins/lighttable/collect/history_pos0", table->offset);
    if (redraw)
        dt_thumbtable_full_redraw(table, TRUE);
    return TRUE;
}

// set offset at specific imgid and redraw if needed
gboolean dt_thumbtable_set_offset_image(dt_thumbtable_t *table, const dt_imgid_t imgid,
                                        const gboolean redraw)
{
    table->offset_imgid = imgid;
    return dt_thumbtable_set_offset(table, _thumb_get_rowid(imgid), redraw);
}

static void _accel_copy(dt_action_t *action)
{
    dt_history_copy(dt_act_on_get_main_image());
}

static void _accel_copy_parts(dt_action_t *action)
{
    dt_history_copy_parts(dt_act_on_get_main_image());
}

static void _accel_paste(dt_action_t *action)
{
    GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    dt_dev_undo_start_record(darktable.develop);
    dt_control_paste_history(imgs);
    dt_dev_undo_end_record(darktable.develop);
}

static void _accel_paste_parts(dt_action_t *action)
{
    GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    dt_dev_undo_start_record(darktable.develop);
    dt_control_paste_parts_history(imgs);
    dt_dev_undo_end_record(darktable.develop);
}

static void _accel_hist_discard(dt_action_t *action)
{
    GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    dt_control_discard_history(imgs);
}

static void _accel_hist_compress(dt_action_t *action)
{
    GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
    dt_control_compress_history(imgs);
}

static void _accel_write_sidecar_files(dt_action_t *action)
{
    (void)action;
    dt_control_write_sidecar_files();
}

static void _accel_duplicate(dt_action_t *action)
{
    dt_undo_start_group(darktable.undo, DT_UNDO_DUPLICATE);

    const dt_imgid_t sourceid = dt_act_on_get_main_image();
    const dt_imgid_t newimgid = dt_image_duplicate(sourceid);
    if (!dt_is_valid_imgid(newimgid))
        return;

    if (strcmp(action->id, "duplicate image"))
        dt_history_delete_on_image(newimgid);
    else
        dt_history_copy_and_paste_on_image(sourceid, newimgid, FALSE, NULL, TRUE, TRUE, TRUE);

    // a duplicate should keep the change time stamp of the original
    dt_image_cache_set_change_timestamp_from_image(newimgid, sourceid);

    dt_undo_end_group(darktable.undo);

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_UNDEF, NULL);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
}

static void _accel_select_all(dt_action_t *action)
{
    dt_selection_select_all(darktable.selection);
}

static void _accel_select_none(dt_action_t *action)
{
    dt_selection_clear(darktable.selection);
}

static void _accel_select_invert(dt_action_t *action)
{
    dt_selection_invert(darktable.selection);
}

static void _accel_select_film(dt_action_t *action)
{
    dt_selection_select_filmroll(darktable.selection);
}

static void _accel_select_untouched(dt_action_t *action)
{
    dt_selection_select_unaltered(darktable.selection);
}

static void _accel_remove_images(dt_action_t *action)
{
    (void)action;
    dt_control_remove_images();
}

static void _accel_delete_images(dt_action_t *action)
{
    (void)action;
    dt_control_delete_images();
}

static void _accel_move_images(dt_action_t *action)
{
    (void)action;
    dt_control_move_images();
}

static void _accel_copy_images(dt_action_t *action)
{
    (void)action;
    dt_control_copy_images();
}

static void _accel_merge_hdr(dt_action_t *action)
{
    (void)action;
    dt_control_merge_hdr();
}

static void _accel_duplicate_images(dt_action_t *action)
{
    dt_control_duplicate_images(g_strcmp0(action->id, "duplicate selected images virgin") == 0);
}

static void _accel_rotate_images(dt_action_t *action)
{
    const int32_t rotation = g_str_has_suffix(action->id, "CCW") ? 1 :
                             g_str_has_suffix(action->id, "CW")  ? 0 :
                                                                      2;
    dt_control_flip_images(rotation);
}

static void _accel_local_copy_images(dt_action_t *action)
{
    if (!g_strcmp0(action->id, "copy selected images locally"))
        dt_control_set_local_copy_images();
    else
        dt_control_reset_local_copy_images();
}

static void _accel_refresh_exif(dt_action_t *action)
{
    (void)action;
    dt_control_refresh_exif();
}

static void _accel_set_monochrome(dt_action_t *action)
{
    dt_control_monochrome_images(!g_strcmp0(action->id, "set selected images monochrome") ? 2 : 0);
}

static void _accel_group_images(dt_action_t *action)
{
    (void)action;
    dt_control_group_images();
}

static void _accel_ungroup_images(dt_action_t *action)
{
    (void)action;
    dt_control_ungroup_images();
}

static void _accel_copy_metadata(dt_action_t *action)
{
    (void)action;
    dt_control_copy_metadata_source();
}

static void _accel_paste_metadata(dt_action_t *action)
{
    (void)action;
    dt_control_paste_metadata();
}

static void _accel_clear_metadata(dt_action_t *action)
{
    (void)action;
    dt_control_clear_metadata();
}

static void _disable_thumb_action(dt_action_status_t *status, const gchar *reason)
{
    status->enabled = FALSE;
    status->reason = reason;
}

static gboolean _thumb_context_has_grouped_image(void)
{
    GList *images = dt_act_on_get_images(FALSE, TRUE, FALSE);
    gboolean grouped = FALSE;
    for (const GList *l = images; l && !grouped; l = g_list_next(l))
    {
        GList *group = dt_grouping_get_group_images(GPOINTER_TO_INT(l->data));
        grouped = g_list_length(group) > 1;
        g_list_free(group);
    }
    g_list_free(images);
    return grouped;
}

static gboolean _thumb_context_all_raw_images(void)
{
    GList *images = dt_act_on_get_images(FALSE, TRUE, FALSE);
    gboolean all_raw = images != NULL;
    for (const GList *l = images; l && all_raw; l = g_list_next(l))
    {
        const dt_image_t *image = dt_image_cache_get(GPOINTER_TO_INT(l->data), 'r');
        all_raw = image && dt_image_is_raw(image);
        if (image)
            dt_image_cache_read_release(image);
    }
    g_list_free(images);
    return all_raw;
}

static gboolean _thumb_context_source_directories_writable(void)
{
    GList *images = dt_act_on_get_images(FALSE, TRUE, FALSE);
    gboolean writable = images != NULL;
    for (const GList *l = images; l && writable; l = g_list_next(l))
    {
        char path[PATH_MAX] = {0};
        gboolean from_cache = TRUE;
        dt_image_full_path(GPOINTER_TO_INT(l->data), path, sizeof(path), &from_cache);
        gchar *directory = path[0] ? g_path_get_dirname(path) : NULL;
        writable = directory && g_access(directory, W_OK | X_OK) == 0;
        g_free(directory);
    }
    g_list_free(images);
    return writable;
}

static gboolean _thumb_context_has_blocking_job(void)
{
    const dt_lib_module_t *progress = darktable.control->progress_system.proxy.module;
    return progress && progress->widget && GTK_IS_WIDGET(progress->widget) &&
           gtk_widget_has_grab(progress->widget);
}

static void _thumb_action_status(const dt_action_t *action, const int instance, const int element,
                                 const int effect, dt_action_status_t *status, gpointer user_data)
{
    (void)instance;
    (void)element;
    (void)effect;
    (void)user_data;

    const int images = dt_act_on_get_images_nb(TRUE, FALSE);
    if (!images)
    {
        _disable_thumb_action(status, _("no images selected"));
        return;
    }

    if (_thumb_context_has_blocking_job())
    {
        _disable_thumb_action(status, _("a blocking task is running"));
        return;
    }

    if (!g_strcmp0(action->id, "copy history") ||
        !g_strcmp0(action->id, "copy history parts") ||
        !g_strcmp0(action->id, "copy selected image metadata"))
    {
        if (images != 1)
            _disable_thumb_action(status, _("select exactly one image"));
        return;
    }

    if (!g_strcmp0(action->id, "paste history") ||
        !g_strcmp0(action->id, "paste history parts"))
    {
        const dt_imgid_t copied = darktable.view_manager->copy_paste.copied_imageid;
        if (!dt_is_valid_imgid(copied))
            _disable_thumb_action(status, _("no copied history"));
        else if (images == 1 && copied == dt_act_on_get_main_image())
            _disable_thumb_action(status, _("history source is selected"));
        return;
    }

    if (!g_strcmp0(action->id, "paste selected image metadata") &&
        !dt_control_can_paste_metadata())
    {
        _disable_thumb_action(status, _("no compatible copied metadata"));
        return;
    }

    if ((!g_strcmp0(action->id, "delete selected images") ||
         !g_strcmp0(action->id, "move selected images") ||
         !g_strcmp0(action->id, "write sidecar files")) &&
        !_thumb_context_source_directories_writable())
    {
        _disable_thumb_action(status, _("a selected image directory is read-only"));
        return;
    }

    if (!g_strcmp0(action->id, "create HDR from selected images"))
    {
        if (images < 2)
            _disable_thumb_action(status, _("select at least two images"));
        else if (!_thumb_context_all_raw_images())
            _disable_thumb_action(status, _("HDR merge requires raw images"));
    }
    else if (!g_strcmp0(action->id, "group selected images") && images < 2)
        _disable_thumb_action(status, _("select at least two images"));
    else if (!g_strcmp0(action->id, "ungroup selected images") &&
             !_thumb_context_has_grouped_image())
        _disable_thumb_action(status, _("no grouped images selected"));
}

static dt_action_t *_register_thumb_image_action(dt_action_t *owner, const gchar *label,
                                                  dt_action_callback_t callback, const guint key,
                                                  const GdkModifierType mods)
{
    dt_action_t *action = dt_action_register(owner, label, callback, key, mods);
    dt_action_set_status_callback(action, _thumb_action_status, NULL);
    return action;
}

// init all accels
static void _thumbtable_init_accels()
{
    static gboolean initialized = FALSE;
    if (initialized)
        return;
    initialized = TRUE;

    dt_action_t *thumb_actions = &darktable.control->actions_thumb;

    /* setup history key accelerators */
    _register_thumb_image_action(thumb_actions, N_("copy history"), _accel_copy, GDK_KEY_c,
                                 GDK_CONTROL_MASK);
    _register_thumb_image_action(thumb_actions, N_("copy history parts"), _accel_copy_parts,
                                 GDK_KEY_c, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    _register_thumb_image_action(thumb_actions, N_("paste history"), _accel_paste, GDK_KEY_v,
                                 GDK_CONTROL_MASK);
    _register_thumb_image_action(thumb_actions, N_("paste history parts"), _accel_paste_parts,
                                 GDK_KEY_v, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    _register_thumb_image_action(thumb_actions, N_("compress history"), _accel_hist_compress, 0,
                                 0);
    _register_thumb_image_action(thumb_actions, N_("discard history"), _accel_hist_discard, 0,
                                 0);
    _register_thumb_image_action(thumb_actions, N_("write sidecar files"),
                                 _accel_write_sidecar_files, 0, 0);

    dt_action_register(thumb_actions, N_("duplicate image"), _accel_duplicate, GDK_KEY_d,
                       GDK_CONTROL_MASK);
    dt_action_register(thumb_actions, N_("duplicate image virgin"), _accel_duplicate, GDK_KEY_d,
                       GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    /* Image operations are registered here as well as in their panel widgets so that a
       thumbnail context menu never needs to call a button callback directly. */
    _register_thumb_image_action(thumb_actions, N_("create HDR from selected images"),
                                 _accel_merge_hdr, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("duplicate selected images"),
                                 _accel_duplicate_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("duplicate selected images virgin"),
                                 _accel_duplicate_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("move selected images"), _accel_move_images, 0,
                                 0);
    _register_thumb_image_action(thumb_actions, N_("copy selected images"), _accel_copy_images, 0,
                                 0);
    _register_thumb_image_action(thumb_actions, N_("remove selected images from library"),
                                 _accel_remove_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("delete selected images"), _accel_delete_images,
                                 0, 0);
    _register_thumb_image_action(thumb_actions, N_("rotate selected images 90 degrees CCW"),
                                 _accel_rotate_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("rotate selected images 90 degrees CW"),
                                 _accel_rotate_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("reset selected images rotation"),
                                 _accel_rotate_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("copy selected images locally"),
                                 _accel_local_copy_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("resync selected images local copy"),
                                 _accel_local_copy_images, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("refresh selected images EXIF"),
                                 _accel_refresh_exif, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("set selected images monochrome"),
                                 _accel_set_monochrome, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("set selected images color"),
                                 _accel_set_monochrome, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("group selected images"), _accel_group_images,
                                 GDK_KEY_g, GDK_CONTROL_MASK);
    _register_thumb_image_action(thumb_actions, N_("ungroup selected images"),
                                 _accel_ungroup_images, GDK_KEY_g,
                                 GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    _register_thumb_image_action(thumb_actions, N_("copy selected image metadata"),
                                 _accel_copy_metadata, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("paste selected image metadata"),
                                 _accel_paste_metadata, 0, 0);
    _register_thumb_image_action(thumb_actions, N_("clear selected image metadata"),
                                 _accel_clear_metadata, 0, 0);

    /* setup selection accelerators */
    dt_action_register(thumb_actions, N_("select all"), _accel_select_all, GDK_KEY_a,
                       GDK_CONTROL_MASK);
    dt_action_register(thumb_actions, N_("select none"), _accel_select_none, GDK_KEY_a,
                       GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    dt_action_register(thumb_actions, N_("invert selection"), _accel_select_invert, GDK_KEY_i,
                       GDK_CONTROL_MASK);
    dt_action_register(thumb_actions, N_("select film roll"), _accel_select_film, 0, 0);
    dt_action_register(thumb_actions, N_("select untouched"), _accel_select_untouched, 0, 0);
}

static gboolean _filemanager_ensure_rowid_visibility(dt_thumbtable_t *table, int rowid)
{
    if (rowid < 1)
        rowid = 1;

    if (!table->list)
        return FALSE;

    // get first and last fully visible thumbnails
    dt_thumbnail_t *first = table->list->data;
    const int pos =
        MIN(g_list_length(table->list) - 1, table->thumbs_per_row * (table->rows - 1) - 1);
    dt_thumbnail_t *last = g_list_nth_data(table->list, pos);

    if (first->rowid > rowid)
    {
        const int rows = MAX(1, (first->rowid - rowid) / table->thumbs_per_row);
        if (_move(table, 0, rows * table->thumb_size, TRUE))
            return _filemanager_ensure_rowid_visibility(table, rowid);
        else
            return FALSE;
    }
    else if (last->rowid < rowid)
    {
        const int rows = MAX(1, (rowid - last->rowid) / table->thumbs_per_row);
        if (_move(table, 0, -rows * table->thumb_size, TRUE))
            return _filemanager_ensure_rowid_visibility(table, rowid);
        else
            return FALSE;
    }
    return TRUE;
}
gboolean dt_thumbtable_ensure_imgid_visibility(dt_thumbtable_t *table, const dt_imgid_t imgid)
{
    if (!dt_is_valid_imgid(imgid))
        return FALSE;
    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        return _filemanager_ensure_rowid_visibility(table, _thumb_get_rowid(imgid));
    else if (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        return _filmstrip_ensure_imgid_visibility(table, imgid);

    return FALSE;
}

static gboolean _filemanager_check_rowid_visibility(dt_thumbtable_t *table, const int rowid)
{
    if (rowid < 1)
        return FALSE;
    if (!table->list)
        return FALSE;
    // get first and last fully visible thumbnails
    dt_thumbnail_t *first = table->list->data;
    const int pos =
        MIN(g_list_length(table->list) - 1, table->thumbs_per_row * (table->rows - 1) - 1);
    dt_thumbnail_t *last = g_list_nth_data(table->list, pos);

    if (first->rowid <= rowid && last->rowid >= rowid)
        return TRUE;
    return FALSE;
}

gboolean dt_thumbtable_check_imgid_visibility(dt_thumbtable_t *table, const dt_imgid_t imgid)
{
    if (!dt_is_valid_imgid(imgid))
        return FALSE;

    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        return _filemanager_check_rowid_visibility(table, _thumb_get_rowid(imgid));

    return FALSE;
}

static gboolean _filemanager_key_move(dt_thumbtable_t *table, const dt_thumbtable_move_t move,
                                      const gboolean select)
{
    // base point
    dt_imgid_t baseid = dt_control_get_mouse_over_id();
    const gboolean first_move = (baseid <= 0);
    int newrowid = -1;
    // let's be sure that the current image is selected
    if (dt_is_valid_imgid(baseid) && select)
        dt_selection_select(darktable.selection, baseid);

    int baserowid = 1;

    // only initialize starting position but do not move yet, if moving
    // for first time...
    if (first_move)
    {
        newrowid = table->offset;
        baseid = table->offset_imgid;
    }
    // ... except for PAGEUP/PAGEDOWN or skipping to the start/end of
    // collection
    if (!first_move || move == DT_THUMBTABLE_MOVE_PAGEUP || move == DT_THUMBTABLE_MOVE_PAGEDOWN ||
        move == DT_THUMBTABLE_MOVE_START || move == DT_THUMBTABLE_MOVE_END)
    {
        baserowid = _thumb_get_rowid(baseid);
        newrowid = baserowid;
        // last rowid of the current collection
        int maxrowid = 1;
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT MAX(rowid) FROM memory.collected_images", -1, &stmt,
                                    NULL);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            maxrowid = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        switch (move)
        {
        // classic keys
        case DT_THUMBTABLE_MOVE_LEFT:
            newrowid = MAX(baserowid - 1, 1);
            break;
        case DT_THUMBTABLE_MOVE_RIGHT:
            newrowid = MIN(baserowid + 1, maxrowid);
            break;
        case DT_THUMBTABLE_MOVE_UP:
            newrowid = MAX(baserowid - table->thumbs_per_row, 1);
            break;
        case DT_THUMBTABLE_MOVE_DOWN:
            newrowid = MIN(baserowid + table->thumbs_per_row, maxrowid);
            break;

        // page keys
        case DT_THUMBTABLE_MOVE_PAGEUP:
            newrowid = baserowid - table->thumbs_per_row * (table->rows - 1);
            while (newrowid < 1)
                newrowid += table->thumbs_per_row;
            if (newrowid == baserowid)
                newrowid = 1;
            break;
        case DT_THUMBTABLE_MOVE_PAGEDOWN:
            newrowid = baserowid + table->thumbs_per_row * (table->rows - 1);
            while (newrowid > maxrowid)
                newrowid -= table->thumbs_per_row;
            if (newrowid == baserowid)
                newrowid = maxrowid;
            break;

        // direct start/end
        case DT_THUMBTABLE_MOVE_START:
            newrowid = 1;
            break;
        case DT_THUMBTABLE_MOVE_END:
            newrowid = maxrowid;
            break;
        default:
            break;
        }
    }

    // change image_over
    const dt_imgid_t imgid = _thumb_get_imgid(newrowid);

    dt_control_set_mouse_over_id(imgid);

    // ensure the image is visible by moving the view if needed
    if (newrowid != -1)
        _filemanager_ensure_rowid_visibility(table, newrowid);

    // if needed, we set the selection
    if (select && dt_is_valid_imgid(imgid))
        dt_selection_select_range(darktable.selection, imgid);
    return TRUE;
}

gboolean dt_thumbtable_key_move(dt_thumbtable_t *table, const dt_thumbtable_move_t move,
                                const gboolean select)
{
    if (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        return _filemanager_key_move(table, move, select);

    return FALSE;
}

gboolean dt_thumbtable_reset_first_offset(dt_thumbtable_t *table)
{
    // nothing to do if thumbtable is empty
    if (!table->list)
        return FALSE;

    if (table->mode != DT_THUMBTABLE_MODE_FILEMANAGER)
        return FALSE;

    // chained dereference is dangerous, but there was a check above in the code
    const dt_thumbnail_t *first = table->list->data;
    const int offset = table->thumbs_per_row - ((first->rowid - 1) % table->thumbs_per_row);
    if (offset == 0)
        return FALSE;

    // we scroll up the list by the number offset
    dt_thumbtable_set_offset(table, table->offset + offset, TRUE);
    return TRUE;
}
