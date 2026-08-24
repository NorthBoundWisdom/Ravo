/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

#include <gui/gtk.h>

// cache structure
typedef struct dt_act_on_cache_t
{
    GList *images;
    int images_nb;
    gboolean ok;
    dt_imgid_t image_over;
    gboolean inside_table;
    GSList *active_imgs;
    gboolean image_over_inside_sel;
    gboolean ordered;
} dt_act_on_cache_t;

typedef enum dt_act_on_algorithm_t
{
    DT_ACT_ON_HOVER,
    DT_ACT_ON_SELECTION
} dt_act_on_algorithm_t;

typedef struct dt_act_on_context_t dt_act_on_context_t;

// get the algorithm used to get the act_on images
dt_act_on_algorithm_t dt_act_on_get_algorithm();

// does the algorithm use culling specific selection
gboolean dt_act_on_use_culling_selection();

// get images to act on for globals change (via libs or accels)
// The list needs to be freed by the caller
GList *dt_act_on_get_images(const gboolean only_visible, const gboolean force,
                            const gboolean ordered);
gchar *dt_act_on_get_query(const gboolean only_visible);

// get the main image to act on during global changes (libs, accels)
dt_imgid_t dt_act_on_get_main_image();

// get only the number of images to act on
int dt_act_on_get_images_nb(const gboolean only_visible, const gboolean force);

/** Temporarily bind actions to a stable image snapshot.  The context is
 * main-thread only and must be popped before returning to the GTK loop. */
dt_act_on_context_t *dt_act_on_push_context(const GList *images, dt_imgid_t main_image);
void dt_act_on_pop_context(dt_act_on_context_t *context);

// reset the cache
void dt_act_on_reset_cache(const gboolean only_visible);

// set the right class for the widget
void dt_act_on_set_class(GtkWidget *widget);
