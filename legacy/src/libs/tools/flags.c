/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "common/act_on.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/undo.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef enum dt_lib_flag_state_t
{
    DT_LIB_FLAG_PICK,
    DT_LIB_FLAG_UNFLAG,
    DT_LIB_FLAG_REJECT,
} dt_lib_flag_state_t;

typedef struct dt_undo_flags_t
{
    dt_imgid_t imgid;
    uint32_t before;
    uint32_t after;
} dt_undo_flags_t;

static const uint32_t _flag_mask = DT_IMAGE_PICKED | DT_IMAGE_REJECTED;

const char *name(dt_lib_module_t *self)
{
    return _("flags");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
    return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
    return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT;
}

gboolean expandable(dt_lib_module_t *self)
{
    return FALSE;
}

int position(const dt_lib_module_t *self)
{
    return 1000;
}

static uint32_t _flag_state_get(const dt_imgid_t imgid)
{
    uint32_t flags = 0;
    const dt_image_t *image = dt_image_cache_get(imgid, 'r');
    if (image)
    {
        flags = image->flags & _flag_mask;
        dt_image_cache_read_release(image);
    }
    return flags;
}

static void _flag_state_set(const dt_imgid_t imgid, const uint32_t state)
{
    dt_image_t *image = dt_image_cache_get(imgid, 'w');
    if (image)
    {
        image->flags = (image->flags & ~_flag_mask) | (state & _flag_mask);
        dt_image_cache_write_release_info(image, DT_IMAGE_CACHE_SAFE, "_flag_state_set");
    }
}

static void _flags_undo(gpointer user_data, const dt_undo_type_t type, const dt_undo_data_t data,
                        const dt_undo_action_t action, GList **imgs)
{
    if (type != DT_UNDO_FLAGS)
        return;

    for (const GList *l = data; l; l = g_list_next(l))
    {
        const dt_undo_flags_t *undo = l->data;
        _flag_state_set(undo->imgid, action == DT_ACTION_UNDO ? undo->before : undo->after);
        *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undo->imgid));
    }

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_NEW_VALUE);
}

static void _flags_undo_data_free(gpointer data)
{
    g_list_free_full(data, g_free);
}

static void _flags_apply(const dt_lib_flag_state_t state)
{
    GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
    if (g_list_is_empty(imgs))
    {
        dt_control_log(_("no images selected to apply flag"));
        return;
    }

    dt_gui_cursor_set_busy();
    GList *undo = NULL;
    dt_undo_start_group(darktable.undo, DT_UNDO_FLAGS);

    for (const GList *l = imgs; l; l = g_list_next(l))
    {
        const dt_imgid_t imgid = GPOINTER_TO_INT(l->data);
        const uint32_t before = _flag_state_get(imgid);
        uint32_t after = before;

        switch (state)
        {
        case DT_LIB_FLAG_PICK:
            after = (before | DT_IMAGE_PICKED) & ~DT_IMAGE_REJECTED;
            break;
        case DT_LIB_FLAG_UNFLAG:
            after = before & ~_flag_mask;
            break;
        case DT_LIB_FLAG_REJECT:
            after = (before | DT_IMAGE_REJECTED) & ~DT_IMAGE_PICKED;
            break;
        }

        if (before == after)
            continue;

        _flag_state_set(imgid, after);
        dt_undo_flags_t *record = g_malloc(sizeof(dt_undo_flags_t));
        record->imgid = imgid;
        record->before = before;
        record->after = after;
        undo = g_list_prepend(undo, record);
    }

    if (undo)
        dt_undo_record(darktable.undo, NULL, DT_UNDO_FLAGS, undo, _flags_undo, _flags_undo_data_free);
    dt_undo_end_group(darktable.undo);

    dt_gui_cursor_clear_busy();
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_NEW_VALUE);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_RATING_RANGE, imgs);
    dt_control_queue_redraw_center();
    g_list_free(imgs);
}

static void _flags_button_clicked(GtkWidget *widget, gpointer user_data)
{
    _flags_apply(GPOINTER_TO_INT(user_data));
    (void)widget;
}

void gui_init(dt_lib_module_t *self)
{
    self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    const struct
    {
        dt_lib_flag_state_t state;
        int paint_flags;
        const char *tooltip;
        const char *action;
    } buttons[] = {
        {DT_LIB_FLAG_PICK, CPF_ACTIVE, N_("pick selected images"), N_("pick")},
        {DT_LIB_FLAG_UNFLAG, CPF_NONE, N_("clear flags on selected images"), N_("unflag")},
        {DT_LIB_FLAG_REJECT, CPF_SPECIAL_FLAG, N_("reject selected images"), N_("reject")},
    };

    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++)
    {
        GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_flag, buttons[i].paint_flags, NULL);
        gtk_widget_set_tooltip_text(button, _(buttons[i].tooltip));
        gtk_box_pack_start(GTK_BOX(self->widget), button, FALSE, FALSE, 0);
        g_signal_connect(button, "clicked", G_CALLBACK(_flags_button_clicked),
                         GINT_TO_POINTER(buttons[i].state));
        dt_action_define(&darktable.control->actions_thumb, NULL, _(buttons[i].action), button,
                         &dt_action_def_button);
    }

    gtk_widget_set_name(self->widget, "lib-flags");
}

void gui_cleanup(dt_lib_module_t *self)
{
}
