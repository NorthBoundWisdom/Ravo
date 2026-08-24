/*
    This file is part of darktable,
    Copyright (C) 2013-2026 darktable developers.

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

#include "develop/masks.h"
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "gui/accelerators.h"
#include "gui/context_menu.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wshadow"
#endif

static void _lib_masks_recreate_list(dt_lib_module_t *self);
static void _lib_masks_update_list(dt_lib_module_t *self);

typedef struct dt_lib_masks_t
{
    /* vbox with managed history items */
    GtkWidget *bt_circle, *bt_path, *bt_gradient, *bt_ellipse, *bt_brush;
    GtkWidget *treeview;
    dt_gui_collapsible_section_t cs;
    GtkWidget *property[DT_MASKS_PROPERTY_LAST];
    GtkWidget *pressure, *smoothing;
    dt_action_t *tree_action;
    float last_value[DT_MASKS_PROPERTY_LAST];
    GtkWidget *none_label;

    GdkPixbuf *ic_inverse, *ic_union, *ic_intersection;
    GdkPixbuf *ic_difference, *ic_sum, *ic_exclusion, *ic_used;
} dt_lib_masks_t;

typedef enum dt_masks_tree_action_t
{
    DT_MASKS_TREE_ACTION_ADD_BRUSH,
    DT_MASKS_TREE_ACTION_ADD_CIRCLE,
    DT_MASKS_TREE_ACTION_ADD_ELLIPSE,
    DT_MASKS_TREE_ACTION_ADD_PATH,
    DT_MASKS_TREE_ACTION_ADD_GRADIENT,
    DT_MASKS_TREE_ACTION_ADD_EXISTING,
    DT_MASKS_TREE_ACTION_DUPLICATE,
    DT_MASKS_TREE_ACTION_DELETE,
    DT_MASKS_TREE_ACTION_GROUP,
    DT_MASKS_TREE_ACTION_INVERSE,
    DT_MASKS_TREE_ACTION_UNION,
    DT_MASKS_TREE_ACTION_INTERSECTION,
    DT_MASKS_TREE_ACTION_DIFFERENCE,
    DT_MASKS_TREE_ACTION_SUM,
    DT_MASKS_TREE_ACTION_EXCLUSION,
    DT_MASKS_TREE_ACTION_MOVE_UP,
    DT_MASKS_TREE_ACTION_MOVE_DOWN,
    DT_MASKS_TREE_ACTION_CLEANUP,
} dt_masks_tree_action_t;

typedef struct dt_masks_tree_context_t
{
    dt_masks_tree_action_t action;
    GList *rows;
    dt_mask_id_t existing_formid;
} dt_masks_tree_context_t;

enum
{
    DT_MASKS_TREE_ACTION_COMMAND = DT_ACTION_ELEMENT_DEFAULT,
    DT_MASKS_TREE_ACTION_OPERATION,
};

static void _free_masks_tree_context(gpointer data)
{
    dt_masks_tree_context_t *context = data;
    if (!context)
        return;

    g_list_free_full(context->rows, (GDestroyNotify)gtk_tree_row_reference_free);
    g_free(context);
}

static dt_masks_tree_context_t *_new_masks_tree_context(dt_lib_module_t *self,
                                                         const dt_masks_tree_action_t action,
                                                         const dt_mask_id_t existing_formid)
{
    dt_lib_masks_t *d = self->data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
    GtkTreeModel *model = NULL;
    GList *selected = gtk_tree_selection_get_selected_rows(selection, &model);
    dt_masks_tree_context_t *context = g_new0(dt_masks_tree_context_t, 1);
    context->action = action;
    context->existing_formid = existing_formid;

    for (const GList *iter = selected; iter; iter = g_list_next(iter))
    {
        GtkTreeRowReference *row = gtk_tree_row_reference_new(model, iter->data);
        if (row)
            context->rows = g_list_append(context->rows, row);
    }
    g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    return context;
}

static void _bind_masks_tree_item(dt_lib_module_t *self, GtkWidget *item,
                                  const dt_masks_tree_action_t action,
                                  const dt_mask_id_t existing_formid)
{
    dt_lib_masks_t *d = self->data;
    dt_masks_tree_context_t *context =
        _new_masks_tree_context(self, action, existing_formid);
    const dt_action_element_t element =
        action >= DT_MASKS_TREE_ACTION_INVERSE && action <= DT_MASKS_TREE_ACTION_EXCLUSION ?
            DT_MASKS_TREE_ACTION_OPERATION :
            DT_MASKS_TREE_ACTION_COMMAND;
    dt_gui_context_menu_bind_action_item(GTK_MENU_ITEM(item), d->tree_action, 0, element,
                                         DT_ACTION_EFFECT_DEFAULT_KEY, context,
                                         _free_masks_tree_context);
}

const char *name(dt_lib_module_t *self)
{
    return _("mask manager");
}

const char *description(dt_lib_module_t *self)
{
    return _("manipulate the drawn shapes used\n"
             "for masks on the processing modules");
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
    return 10;
}

typedef enum dt_masks_tree_cols_t
{
    TREE_TEXT = 0,
    TREE_MODULE,
    TREE_GROUPID,
    TREE_FORMID,
    TREE_EDITABLE,
    TREE_IC_OP,
    TREE_IC_OP_VISIBLE,
    TREE_IC_INVERSE,
    TREE_IC_INVERSE_VISIBLE,
    TREE_IC_USED,
    TREE_IC_USED_VISIBLE,
    TREE_USED_TEXT,
    TREE_COUNT
} dt_masks_tree_cols_t;

// boolean = TRUE renders as a checkbox; min/max/relative are unused
const struct
{
    gchar *name;
    gchar *format;
    float min, max;
    gboolean relative;
    gboolean boolean;
} _masks_properties[DT_MASKS_PROPERTY_LAST] = {
    [DT_MASKS_PROPERTY_OPACITY] = {N_("opacity"), "%", 0, 1, FALSE, FALSE},
    [DT_MASKS_PROPERTY_SIZE] = {N_("size"), "%", 0.0001, 1, TRUE, FALSE},
    [DT_MASKS_PROPERTY_HARDNESS] = {N_("hardness"), "%", 0.0001, 1, TRUE, FALSE},
    [DT_MASKS_PROPERTY_FEATHER] = {N_("feather"), "%", 0.0001, 1, TRUE, FALSE},
    [DT_MASKS_PROPERTY_ROTATION] = {N_("rotation"), "°", 0, 360, FALSE, FALSE},
    [DT_MASKS_PROPERTY_CURVATURE] = {N_("curvature"), "%", -1, 1, FALSE, FALSE},
    [DT_MASKS_PROPERTY_COMPRESSION] = {N_("compression"), "%", 0.0001, 1, TRUE, FALSE},
};

gboolean _timeout_show_all_feathers(gpointer userdata)
{
    dt_masks_form_gui_t *gui = userdata;
    gui->show_all_feathers = 0;
    dt_control_queue_redraw_center();
    return G_SOURCE_REMOVE;
}

static void _property_changed(GtkWidget *widget, dt_masks_property_t prop)
{
    dt_lib_module_t *self = darktable.develop->proxy.masks.module;
    dt_lib_masks_t *d = self->data;
    dt_develop_t *dev = darktable.develop;
    dt_masks_form_t *form = dev->form_visible;
    dt_masks_form_gui_t *gui = dev->form_gui;
    if (!form || !gui)
    {
        gtk_widget_hide(widget);
        return;
    }

    const gboolean is_bool = _masks_properties[prop].boolean;
    const float value = is_bool ? (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) :
                                  dt_bauhaus_slider_get(widget);

    DT_ENTER_GUI_UPDATE();
    int count = 0, pos = 0;
    float sum = 0, min = _masks_properties[prop].min, max = _masks_properties[prop].max;
    if (!is_bool)
    {
        if (_masks_properties[prop].relative)
        {
            max /= min;
            min /= _masks_properties[prop].max;
        }
        else
        {
            max -= min;
            min -= _masks_properties[prop].max;
        }
    }

    if (prop == DT_MASKS_PROPERTY_OPACITY && gui->creation)
    {
        float opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
        opacity = CLAMP(opacity + value - d->last_value[prop], 0.05f, 1.0f);
        dt_conf_set_float("plugins/darkroom/masks/opacity", opacity);
        sum += opacity;
        ++count;
    }
    else if (!(form->type & DT_MASKS_GROUP) && form->functions && form->functions->modify_property)
    {
        form->functions->modify_property(form, prop, d->last_value[prop], value, &sum, &count, &min,
                                         &max);
        if (!gui->creation && value != d->last_value[prop])
            dt_masks_gui_form_create(form, gui, pos, dev->gui_module);
    }
    else
    {
        for (GList *fpts = form->points; fpts; fpts = g_list_next(fpts), pos++)
        {
            dt_masks_point_group_t *fpt = fpts->data;
            dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
            if (!sel || (dev->mask_form_selected_id && dev->mask_form_selected_id != sel->formid))
                continue;
            ;

            if (prop == DT_MASKS_PROPERTY_OPACITY && dt_is_valid_maskid(fpt->parentid))
            {
                const float new_opacity =
                    dt_masks_form_change_opacity(sel, fpt->parentid, value - d->last_value[prop]);
                sum += new_opacity;
                max = fminf(max, 1.0f - new_opacity);
                min = fmaxf(min, .05f - new_opacity);
                ++count;
            }
            else
            {
                const int saved_count = count;

                if (sel->functions && sel->functions->modify_property)
                    sel->functions->modify_property(sel, prop, d->last_value[prop], value, &sum,
                                                    &count, &min, &max);

                if (count != saved_count && value != d->last_value[prop])
                {
                    // we recreate the form points
                    dt_masks_gui_form_create(sel, gui, pos, dev->gui_module);
                }
            }
        }
    }

    gtk_widget_set_visible(widget, count != 0);
    if (count)
    {
        if (value != d->last_value[prop] && sum / count != d->last_value[prop] &&
            prop != DT_MASKS_PROPERTY_OPACITY && !gui->creation)
        {
            if (gui->show_all_feathers)
                g_source_remove(gui->show_all_feathers);

            gui->show_all_feathers = g_timeout_add_seconds(2, _timeout_show_all_feathers, gui);

            // we save the new parameters
            dt_dev_add_masks_history_item(darktable.develop, dev->gui_module, TRUE);
        }

        if (is_bool)
        {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), (sum / count) > 0.5f);
            d->last_value[prop] = (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
        }
        else
        {
            if (_masks_properties[prop].relative)
            {
                max *= sum / count;
                min *= sum / count;
            }
            else
            {
                max += sum / count;
                min += sum / count;
            }

            if (dt_isnan(min))
                min = _masks_properties[prop].min;
            if (dt_isnan(max))
                max = _masks_properties[prop].max;
            dt_bauhaus_slider_set_soft_range(widget, min, max);

            dt_bauhaus_slider_set(widget, sum / count);
            d->last_value[prop] = dt_bauhaus_slider_get(widget);
        }

        gtk_widget_hide(d->none_label);
        dt_control_queue_redraw_center();
    }

    DT_LEAVE_GUI_UPDATE();
}

static void _update_all_properties(dt_lib_masks_t *self)
{
    gtk_widget_show(self->none_label);

    for (int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
        _property_changed(self->property[i], i);

    dt_masks_form_t *form = darktable.develop->form_visible;
    gboolean drawing_brush = form && form->type & DT_MASKS_BRUSH;

    gtk_widget_set_visible(self->pressure, drawing_brush && darktable.gui->have_pen_pressure);
    gtk_widget_set_visible(self->smoothing, drawing_brush);
}

static void _lib_masks_get_values(GtkTreeModel *model, GtkTreeIter *iter, dt_iop_module_t **module,
                                  dt_mask_id_t *groupid, dt_mask_id_t *formid)
{
    // returns module & groupid & formid if requested
    if (module)
        gtk_tree_model_get(model, iter, TREE_MODULE, module, -1);
    if (groupid)
        gtk_tree_model_get(model, iter, TREE_GROUPID, groupid, -1);
    if (formid)
        gtk_tree_model_get(model, iter, TREE_FORMID, formid, -1);
}

static void _lib_masks_inactivate_icons(dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;

    // we set the add shape icons inactive
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_circle), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_ellipse), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_path), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_gradient), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_brush), FALSE);
}

static void _tree_add_shape(GtkButton *button, gpointer shape)
{
    dt_iop_module_t *module = NULL;

    dt_lib_masks_t *lm = darktable.develop->proxy.masks.module->data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    GtkTreeModel *model = NULL;
    GList *selected = gtk_tree_selection_get_selected_rows(selection, &model);
    if (selected)
    {
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, selected->data))
            _lib_masks_get_values(model, &iter, &module, NULL, NULL);
        g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }

    // we create the new form
    dt_masks_form_t *spot = dt_masks_create(GPOINTER_TO_INT(shape));
    dt_masks_change_form_gui(spot);
    darktable.develop->form_gui->creation_module = module;
    darktable.develop->form_gui->group_selected = 0;
    // the new form must be editable
    darktable.develop->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
    dt_control_queue_redraw_center();
}

static gboolean _bt_add_shape(GtkWidget *widget, GdkEventButton *event, gpointer shape)
{
    DT_GUARD_GUI_UPDATE(FALSE);

    if (event->button == GDK_BUTTON_PRIMARY)
    {
        _tree_add_shape(NULL, shape);

        if (dt_modifier_is(event->state, GDK_CONTROL_MASK))
        {
            darktable.develop->form_gui->creation_continuous = TRUE;
            darktable.develop->form_gui->creation_continuous_module =
                darktable.develop->form_gui->creation_module;
        }

        _lib_masks_inactivate_icons(darktable.develop->proxy.masks.module);
    }
    return TRUE;
}

static void _tree_add_exist_to_group(dt_masks_form_t *grp, const dt_mask_id_t id,
                                     dt_iop_module_t *module)
{
    if (!grp || !(grp->type & DT_MASKS_GROUP))
        return;

    // we add the form in this group
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
    if (form && dt_masks_group_add_form(grp, form))
    {
        // we save the group
        dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);

        // and we apply the change
        dt_masks_iop_update(module);
        dt_dev_masks_selection_change(darktable.develop, NULL, grp->formid);
    }
}

static void _tree_group(GtkButton *button, dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;
    // we create the new group
    dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
    snprintf(grp->name, sizeof(grp->name), _("group #%d"), g_list_length(darktable.develop->forms));

    // we add all selected forms to this group
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

    int pos = 0;
    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
    for (GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, item))
        {
            dt_mask_id_t id = INVALID_MASKID;
            _lib_masks_get_values(model, &iter, NULL, NULL, &id);

            if (dt_is_valid_maskid(id))
            {
                dt_masks_point_group_t *fpt = malloc(sizeof(dt_masks_point_group_t));
                fpt->formid = id;
                fpt->parentid = grp->formid;
                fpt->opacity = 1.0f;
                fpt->state = DT_MASKS_STATE_USE;
                if (pos > 0)
                    fpt->state |= DT_MASKS_STATE_UNION;
                grp->points = g_list_append(grp->points, fpt);
                pos++;
            }
        }
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

    // we add this group to the general list
    darktable.develop->forms = g_list_append(darktable.develop->forms, grp);

    // add we save
    dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
    _lib_masks_recreate_list(self);
    // dt_masks_change_form_gui(grp);
}

static void _set_iter_name(dt_lib_masks_t *lm, dt_masks_form_t *form, const int state,
                           const float opacity, GtkTreeModel *model, GtkTreeIter *iter)
{
    if (!form)
        return;

    char str[256] = "";
    g_strlcat(str, form->name, sizeof(str));

    if (opacity != 1.0f)
    {
        char str2[256] = "";
        g_strlcpy(str2, str, sizeof(str2));
        snprintf(str, sizeof(str), "%s %d%%", str2, (int)(opacity * 100));
    }

    const gboolean show = state & DT_MASKS_STATE_SHOW;

    GdkPixbuf *icop = NULL;
    GdkPixbuf *icinv = NULL;

    if (state & DT_MASKS_STATE_UNION)
        icop = lm->ic_union;
    else if (state & DT_MASKS_STATE_INTERSECTION)
        icop = lm->ic_intersection;
    else if (state & DT_MASKS_STATE_DIFFERENCE)
        icop = lm->ic_difference;
    else if (state & DT_MASKS_STATE_SUM)
        icop = lm->ic_sum;
    else if (state & DT_MASKS_STATE_EXCLUSION)
        icop = lm->ic_exclusion;

    if (state & DT_MASKS_STATE_INVERSE)
        icinv = lm->ic_inverse;

    gtk_tree_store_set(GTK_TREE_STORE(model), iter, TREE_TEXT, str, TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (icop != NULL) && show, TREE_IC_INVERSE, icinv,
                       TREE_IC_INVERSE_VISIBLE, (icinv != NULL), -1);
}

static void _tree_cleanup(GtkButton *button, dt_lib_module_t *self)
{
    dt_masks_cleanup_unused(darktable.develop);
    _lib_masks_recreate_list(self);
}

static void _add_masks_history_item(dt_lib_masks_t *lm)
{
    DT_ENTER_GUI_UPDATE();
    dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
    DT_LEAVE_GUI_UPDATE();
}

static void _tree_operation(GtkButton *button, gpointer user_data)
{
    dt_masks_state_t change_state = GPOINTER_TO_INT(user_data);
    dt_lib_module_t *self = darktable.develop->proxy.masks.module;
    dt_lib_masks_t *lm = self->data;

    // now we go through all selected nodes
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    gboolean change = FALSE;
    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

    for (const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        GtkTreeIter iter;

        if (gtk_tree_model_get_iter(model, &iter, item))
        {
            dt_mask_id_t grid = INVALID_MASKID;
            dt_mask_id_t id = INVALID_MASKID;
            _lib_masks_get_values(model, &iter, NULL, &grid, &id);

            dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
            if (grp && (grp->type & DT_MASKS_GROUP))
            {
                // we search the entry to inverse
                for (const GList *pts = grp->points; pts; pts = g_list_next(pts))
                {
                    dt_masks_point_group_t *pt = pts->data;
                    if (pt->formid == id)
                    {
                        if (change_state == DT_MASKS_STATE_INVERSE ||
                            (pt->state & DT_MASKS_STATE_OP && !(pt->state & change_state)))
                        {
                            if (change_state != DT_MASKS_STATE_INVERSE)
                                pt->state &= ~DT_MASKS_STATE_OP;
                            pt->state ^= change_state;
                            _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id),
                                           pt->state, pt->opacity, model, &iter);
                            change = TRUE;
                        }
                        break;
                    }
                }
            }
        }
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

    if (change)
        _add_masks_history_item(lm);
}

static dt_masks_tree_action_t _tree_action_from_mask_state(const dt_masks_state_t state)
{
    switch (state)
    {
    case DT_MASKS_STATE_INVERSE:
        return DT_MASKS_TREE_ACTION_INVERSE;
    case DT_MASKS_STATE_UNION:
        return DT_MASKS_TREE_ACTION_UNION;
    case DT_MASKS_STATE_INTERSECTION:
        return DT_MASKS_TREE_ACTION_INTERSECTION;
    case DT_MASKS_STATE_DIFFERENCE:
        return DT_MASKS_TREE_ACTION_DIFFERENCE;
    case DT_MASKS_STATE_SUM:
        return DT_MASKS_TREE_ACTION_SUM;
    case DT_MASKS_STATE_EXCLUSION:
        return DT_MASKS_TREE_ACTION_EXCLUSION;
    default:
        return DT_MASKS_TREE_ACTION_INVERSE;
    }
}

static void _add_tree_operation(GtkMenuShell *menu, dt_lib_module_t *self, gchar *label,
                                dt_masks_state_t state, dt_masks_state_t selected_states,
                                gboolean sensitive)
{
    GtkWidget *item = gtk_check_menu_item_new_with_label(label);
    if (selected_states & state)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
    _bind_masks_tree_item(self, item, _tree_action_from_mask_state(state), INVALID_MASKID);
    gtk_widget_set_sensitive(item, sensitive);
    gtk_menu_shell_append(menu, item);
}

static void _swap_last_secondlast_item_visibility(dt_lib_masks_t *lm, GtkTreeIter *iter,
                                                  const dt_mask_id_t secondlast_id,
                                                  const dt_mask_id_t last_id)
{
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));

    dt_mask_id_t grid = INVALID_MASKID;
    dt_mask_id_t id = INVALID_MASKID;
    _lib_masks_get_values(model, iter, NULL, &grid, &id);

    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);

    if (grp)
    {
        // we search the entries and change the state
        // the new last entry is removed the SHOW state and
        // the new second last node is set SHOW state + UNION if no operator defined yet.
        for (const GList *pts = g_list_last(grp->points); pts; pts = g_list_previous(pts))
        {
            dt_masks_point_group_t *pt = pts->data;
            gboolean changed = FALSE;
            if (pt->formid == last_id)
            {
                pt->state &= ~DT_MASKS_STATE_SHOW;
                changed = TRUE;
            }
            else if (pt->formid == secondlast_id)
            {
                // ensure that at least an operator is defined as we are
                // going to show this mask operator.
                if ((pt->state & DT_MASKS_STATE_OP) == DT_MASKS_STATE_NONE)
                    pt->state |= DT_MASKS_STATE_UNION;
                pt->state |= DT_MASKS_STATE_SHOW;
                changed = TRUE;
            }
            if (changed)
                _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state,
                               pt->opacity, model, iter);
        }
    }
}

static gboolean _is_last_tree_item(GtkTreeModel *model, GtkTreeIter *iter)
{
    GtkTreeIter *tmp = gtk_tree_iter_copy(iter);
    const gboolean is_last_item = !gtk_tree_model_iter_next(model, tmp);
    gtk_tree_iter_free(tmp);
    return is_last_item;
}

static void _tree_moveup(GtkButton *button, dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;

    dt_masks_clear_form_gui(darktable.develop);

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

    for (const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        GtkTreeIter iter;

        if (gtk_tree_model_get_iter(model, &iter, item))
        {
            dt_mask_id_t grid = INVALID_MASKID;
            dt_mask_id_t id = INVALID_MASKID;
            _lib_masks_get_values(model, &iter, NULL, &grid, &id);

            GtkTreeIter *prev_iter = gtk_tree_iter_copy(&iter);
            if (gtk_tree_model_iter_previous(model, prev_iter))
            {
                dt_mask_id_t prev_grid = INVALID_MASKID;
                dt_mask_id_t prev_id = INVALID_MASKID;
                _lib_masks_get_values(model, prev_iter, NULL, &prev_grid, &prev_id);

                if (_is_last_tree_item(model, &iter))
                {
                    _swap_last_secondlast_item_visibility(lm, &iter, id, prev_id);
                }
            }

            gtk_tree_iter_free(prev_iter);

            dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 1);
        }
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

    dt_dev_add_masks_history_item(darktable.develop, NULL, TRUE);
    _lib_masks_recreate_list(self);
}

static void _tree_movedown(GtkButton *button, dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;

    dt_masks_clear_form_gui(darktable.develop);

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

    for (const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        GtkTreeIter iter;

        if (gtk_tree_model_get_iter(model, &iter, item))
        {
            dt_mask_id_t grid = INVALID_MASKID;
            dt_mask_id_t id = INVALID_MASKID;
            _lib_masks_get_values(model, &iter, NULL, &grid, &id);

            GtkTreeIter *next_iter = gtk_tree_iter_copy(&iter);
            gtk_tree_model_iter_next(model, next_iter);
            dt_mask_id_t next_grid = INVALID_MASKID;
            dt_mask_id_t next_id = INVALID_MASKID;
            _lib_masks_get_values(model, next_iter, NULL, &next_grid, &next_id);

            if (_is_last_tree_item(model, next_iter))
            {
                _swap_last_secondlast_item_visibility(lm, &iter, next_id, id);
            }

            gtk_tree_iter_free(next_iter);

            dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 0);
        }
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

    dt_dev_add_masks_history_item(darktable.develop, NULL, TRUE);
    _lib_masks_recreate_list(self);
}

static void _tree_delete_shape(GtkButton *button, dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;

    dt_masks_clear_form_gui(darktable.develop);

    // now we go through all selected nodes
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    dt_iop_module_t *module = NULL;

    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

    for (const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, item))
        {
            GtkTreeIter *prev_iter = gtk_tree_iter_copy(&iter);
            GtkTreeIter *next_iter = gtk_tree_iter_copy(&iter);
            const gboolean has_previous = gtk_tree_model_iter_previous(model, prev_iter);
            const gboolean has_next = gtk_tree_model_iter_next(model, next_iter);
            dt_mask_id_t prev_grid = INVALID_MASKID;
            dt_mask_id_t prev_id = INVALID_MASKID;

            dt_mask_id_t grid = INVALID_MASKID;
            dt_mask_id_t id = INVALID_MASKID;
            _lib_masks_get_values(model, &iter, &module, &grid, &id);

            if (has_previous)
                gtk_tree_selection_select_iter(selection, prev_iter);
            else if (has_next)
                gtk_tree_selection_select_iter(selection, next_iter);

            if (has_previous)
            {
                _lib_masks_get_values(model, prev_iter, &module, &prev_grid, &prev_id);
                if (_is_last_tree_item(model, &iter))
                {
                    _swap_last_secondlast_item_visibility(lm, &iter, id, prev_id);
                }
            }
            gtk_tree_iter_free(prev_iter);
            gtk_tree_iter_free(next_iter);
            dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, grid),
                                 dt_masks_get_from_id(darktable.develop, id));
        }
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

    dt_dev_add_masks_history_item(darktable.develop, NULL, TRUE);
    _lib_masks_recreate_list(self);
}

static void _tree_duplicate_shape(GtkButton *button, dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;

    // we get the selected node
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
    if (!items)
        return;
    GtkTreePath *item = (GtkTreePath *)items->data;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(model, &iter, item))
    {
        dt_mask_id_t id = INVALID_MASKID;
        _lib_masks_get_values(model, &iter, NULL, NULL, &id);

        const dt_mask_id_t nid = dt_masks_form_duplicate(darktable.develop, id);
        if (dt_is_valid_maskid(nid))
        {
            dt_dev_masks_selection_change(darktable.develop, NULL, nid);
            //_lib_masks_recreate_list(self);
        }
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
}

static gboolean _restore_masks_tree_context(GtkTreeView *tree,
                                            const dt_masks_tree_context_t *context)
{
    GtkTreeModel *model = gtk_tree_view_get_model(tree);
    if (!model)
        return FALSE;

    GList *paths = NULL;
    for (const GList *iter = context->rows; iter; iter = g_list_next(iter))
    {
        GtkTreeRowReference *row = iter->data;
        if (gtk_tree_row_reference_get_model(row) != model)
        {
            g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
            return FALSE;
        }

        GtkTreePath *path = gtk_tree_row_reference_get_path(row);
        if (!path)
        {
            g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
            return FALSE;
        }
        paths = g_list_append(paths, path);
    }

    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree);
    gtk_tree_selection_unselect_all(selection);
    for (const GList *iter = paths; iter; iter = g_list_next(iter))
        gtk_tree_selection_select_path(selection, iter->data);
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return TRUE;
}

static gboolean _get_first_selected_mask_group(GtkTreeView *tree, dt_iop_module_t **module,
                                                dt_mask_id_t *groupid)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(tree);
    GtkTreeModel *model = NULL;
    GList *selected = gtk_tree_selection_get_selected_rows(selection, &model);
    gboolean found = FALSE;
    if (selected)
    {
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, selected->data))
        {
            _lib_masks_get_values(model, &iter, module, groupid, NULL);
            found = TRUE;
        }
    }
    g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    return found;
}

static float _masks_tree_action_process(gpointer target, const dt_action_element_t element,
                                        const dt_action_effect_t effect, const float move_size)
{
    if (!DT_PERFORM_ACTION(move_size) || !GTK_IS_TREE_VIEW(target) ||
        (element != DT_MASKS_TREE_ACTION_COMMAND && element != DT_MASKS_TREE_ACTION_OPERATION) ||
        effect != DT_ACTION_EFFECT_DEFAULT_KEY)
        return DT_ACTION_NOT_VALID;

    dt_action_t *action = dt_action_find_widget(GTK_WIDGET(target), NULL);
    const dt_masks_tree_context_t *context = dt_gui_context_menu_get_action_payload(action);
    dt_lib_module_t *self = g_object_get_data(G_OBJECT(target), "lib-instance");
    if (!context || !self || !self->data ||
        !_restore_masks_tree_context(GTK_TREE_VIEW(target), context))
        return DT_ACTION_NOT_VALID;

    switch (context->action)
    {
    case DT_MASKS_TREE_ACTION_ADD_BRUSH:
        _tree_add_shape(NULL, GINT_TO_POINTER(DT_MASKS_BRUSH));
        break;
    case DT_MASKS_TREE_ACTION_ADD_CIRCLE:
        _tree_add_shape(NULL, GINT_TO_POINTER(DT_MASKS_CIRCLE));
        break;
    case DT_MASKS_TREE_ACTION_ADD_ELLIPSE:
        _tree_add_shape(NULL, GINT_TO_POINTER(DT_MASKS_ELLIPSE));
        break;
    case DT_MASKS_TREE_ACTION_ADD_PATH:
        _tree_add_shape(NULL, GINT_TO_POINTER(DT_MASKS_PATH));
        break;
    case DT_MASKS_TREE_ACTION_ADD_GRADIENT:
        _tree_add_shape(NULL, GINT_TO_POINTER(DT_MASKS_GRADIENT));
        break;
    case DT_MASKS_TREE_ACTION_ADD_EXISTING:
    {
        dt_iop_module_t *module = NULL;
        dt_mask_id_t groupid = INVALID_MASKID;
        if (!_get_first_selected_mask_group(GTK_TREE_VIEW(target), &module, &groupid))
            return DT_ACTION_NOT_VALID;
        _tree_add_exist_to_group(dt_masks_get_from_id(darktable.develop, groupid),
                                 context->existing_formid, module);
        break;
    }
    case DT_MASKS_TREE_ACTION_DUPLICATE:
        _tree_duplicate_shape(NULL, self);
        break;
    case DT_MASKS_TREE_ACTION_DELETE:
        _tree_delete_shape(NULL, self);
        break;
    case DT_MASKS_TREE_ACTION_GROUP:
        _tree_group(NULL, self);
        break;
    case DT_MASKS_TREE_ACTION_INVERSE:
        _tree_operation(NULL, GINT_TO_POINTER(DT_MASKS_STATE_INVERSE));
        break;
    case DT_MASKS_TREE_ACTION_UNION:
        _tree_operation(NULL, GINT_TO_POINTER(DT_MASKS_STATE_UNION));
        break;
    case DT_MASKS_TREE_ACTION_INTERSECTION:
        _tree_operation(NULL, GINT_TO_POINTER(DT_MASKS_STATE_INTERSECTION));
        break;
    case DT_MASKS_TREE_ACTION_DIFFERENCE:
        _tree_operation(NULL, GINT_TO_POINTER(DT_MASKS_STATE_DIFFERENCE));
        break;
    case DT_MASKS_TREE_ACTION_SUM:
        _tree_operation(NULL, GINT_TO_POINTER(DT_MASKS_STATE_SUM));
        break;
    case DT_MASKS_TREE_ACTION_EXCLUSION:
        _tree_operation(NULL, GINT_TO_POINTER(DT_MASKS_STATE_EXCLUSION));
        break;
    case DT_MASKS_TREE_ACTION_MOVE_UP:
        _tree_moveup(NULL, self);
        break;
    case DT_MASKS_TREE_ACTION_MOVE_DOWN:
        _tree_movedown(NULL, self);
        break;
    case DT_MASKS_TREE_ACTION_CLEANUP:
        _tree_cleanup(NULL, self);
        break;
    }

    return 1.0f;
}

static const dt_action_element_def_t _action_elements_masks_tree[] = {
    {NULL, dt_action_effect_activate},
    {NULL, dt_action_effect_toggle},
    {NULL},
};

static const dt_action_def_t _action_def_masks_tree = {N_("selected shapes"),
                                                        _masks_tree_action_process,
                                                        _action_elements_masks_tree};

static void _tree_cell_edited(GtkCellRendererText *cell, gchar *path_string, gchar *new_text,
                              dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_string))
        return;

    dt_mask_id_t id = INVALID_MASKID;
    _lib_masks_get_values(model, &iter, NULL, NULL, &id);
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
    if (!form)
        return;

    // we want to make sure that the new name is not an empty
    // string. else this would convert in the xmp file into "<rdf:li/>"
    // which produces problems. we use a single whitespace as the pure
    // minimum text.
    gchar *text = strlen(new_text) == 0 ? " " : new_text;

    // first, we need to update the mask name

    g_strlcpy(form->name, text, sizeof(form->name));
    dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
}

static void _tree_selection_change(GtkTreeSelection *selection, dt_lib_masks_t *self)
{
    DT_GUARD_GUI_UPDATE();
    // we reset all "show mask" icon of iops
    dt_masks_reset_show_masks_icons();

    const int nb = gtk_tree_selection_count_selected_rows(selection);

    // else, we create a new form group with the selection and display it
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(self->treeview));
    dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);

    for (const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        GtkTreeIter iter;

        if (gtk_tree_model_get_iter(model, &iter, item))
        {
            dt_mask_id_t grid = INVALID_MASKID;
            dt_mask_id_t id = INVALID_MASKID;
            _lib_masks_get_values(model, &iter, NULL, &grid, &id);

            dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
            if (form)
            {
                dt_masks_point_group_t *fpt = malloc(sizeof(dt_masks_point_group_t));
                fpt->formid = id;
                fpt->parentid = grid;
                fpt->state = DT_MASKS_STATE_USE;
                fpt->opacity = 1.0f;
                grp->points = g_list_append(grp->points, fpt);
                // we eventually set the "show masks" icon of iops
                if (nb == 1 && (form->type & DT_MASKS_GROUP))
                {
                    dt_iop_module_t *module = NULL;
                    _lib_masks_get_values(model, &iter, &module, NULL, NULL);

                    if (module && module->blend_data &&
                        (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) &&
                        !(module->flags() & IOP_FLAGS_NO_MASKS))
                    {
                        dt_iop_gui_blend_data_t *bd = module->blend_data;
                        bd->masks_shown = DT_MASKS_EDIT_FULL;
                        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
                        gtk_widget_queue_draw(bd->masks_edit);
                    }
                }
            }
        }
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

    dt_masks_form_t *new_group = dt_masks_create(DT_MASKS_GROUP);
    new_group->formid = NO_MASKID;
    dt_masks_group_ungroup(new_group, grp);

    // don't call dt_masks_change_form_gui because it triggers a selection change again
    dt_masks_clear_form_gui(darktable.develop);
    darktable.develop->form_visible = new_group;

    // update sticky accels window
    if (darktable.view_manager->accels_window.window &&
        darktable.view_manager->accels_window.sticky)
        dt_view_accels_refresh(darktable.view_manager);

    darktable.develop->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
    dt_control_queue_redraw_center();

    _update_all_properties(self);
}

static gboolean _show_tree_context_menu(GtkWidget *treeview, dt_lib_module_t *self)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
    GtkTreeIter iter;
    dt_iop_module_t *module = NULL;

    GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
    GtkWidget *item;

    // we get all infos from selection
    const int nb = gtk_tree_selection_count_selected_rows(selection);
    gboolean from_group = FALSE;

    gboolean is_first_row = FALSE;
    gboolean is_last_row = FALSE;
    dt_masks_state_t selected_states = DT_MASKS_STATE_NONE;

    int grpid = NO_MASKID;
    int depth = 0;
    dt_masks_form_t *grp = NULL;

    if (nb > 0)
    {
        GList *selected = gtk_tree_selection_get_selected_rows(selection, NULL);
        GtkTreePath *it0 = (GtkTreePath *)selected->data;
        depth = gtk_tree_path_get_depth(it0);
        if (nb == 1)
        {
            // before freeing the list of selected rows, we check if the
            // form is a group or not
            if (gtk_tree_model_get_iter(model, &iter, it0))
            {
                _lib_masks_get_values(model, &iter, &module, NULL, &grpid);
                grp = dt_masks_get_from_id(darktable.develop, grpid);
            }

            // if depth > 1 then check if the selected item is the first
            // or last in the group. This is used to enable/disable some
            // feature only meaningful for rows with prev/next.

            GtkTreeIter it;
            GtkTreePath *selected_path = gtk_tree_path_copy(it0);
            gtk_tree_model_get_iter(model, &it, selected_path);
            is_last_row = !gtk_tree_model_iter_next(model, &it);

            if (!is_last_row && !gtk_tree_path_prev(selected_path))
            {
                is_first_row = TRUE;
            }
            gtk_tree_path_free(selected_path);
        }

        for (const GList *items_iter = selected; items_iter; items_iter = g_list_next(items_iter))
        {
            GtkTreePath *selected_path = (GtkTreePath *)items_iter->data;

            if (gtk_tree_model_get_iter(model, &iter, selected_path))
            {
                dt_mask_id_t grid = INVALID_MASKID;
                dt_mask_id_t id = INVALID_MASKID;
                _lib_masks_get_values(model, &iter, NULL, &grid, &id);

                dt_masks_form_t *new_group = dt_masks_get_from_id(darktable.develop, grid);
                if (new_group && (new_group->type & DT_MASKS_GROUP))
                {
                    for (const GList *pts = new_group->points; pts; pts = g_list_next(pts))
                    {
                        dt_masks_point_group_t *pt = pts->data;
                        if (pt->formid == id)
                            selected_states |= pt->state;
                    }
                }
            }
        }

        g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }

    if (depth > 1)
        from_group = TRUE;

    if (nb == 0 || (grp && grp->type & DT_MASKS_GROUP))
    {
        item = gtk_menu_item_new_with_label(_("add brush"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_ADD_BRUSH, INVALID_MASKID);
        gtk_menu_shell_append(menu, item);

        item = gtk_menu_item_new_with_label(_("add circle"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_ADD_CIRCLE, INVALID_MASKID);
        gtk_menu_shell_append(menu, item);

        item = gtk_menu_item_new_with_label(_("add ellipse"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_ADD_ELLIPSE, INVALID_MASKID);
        gtk_menu_shell_append(menu, item);

        item = gtk_menu_item_new_with_label(_("add path"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_ADD_PATH, INVALID_MASKID);
        gtk_menu_shell_append(menu, item);

        item = gtk_menu_item_new_with_label(_("add gradient"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_ADD_GRADIENT, INVALID_MASKID);
        gtk_menu_shell_append(menu, item);
    }

    if (grp && grp->type & DT_MASKS_GROUP)
    {
        // existing forms
        gboolean has_unused_shapes = FALSE;
        GtkWidget *menu0 = gtk_menu_new();

        for (GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
        {
            dt_masks_form_t *form = forms->data;
            if ((form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) || form->formid == grpid)
            {
                continue;
            }
            char str[10000] = "";
            g_strlcat(str, form->name, sizeof(str));
            int nbuse = 0;

            // we search were this form is used
            for (const GList *modules = darktable.develop->iop; modules;
                 modules = g_list_next(modules))
            {
                dt_iop_module_t *m = modules->data;
                dt_masks_form_t *module_group =
                    dt_masks_get_from_id(m->dev, m->blend_params->mask_id);
                if (module_group && (module_group->type & DT_MASKS_GROUP))
                {
                    for (const GList *pts = module_group->points; pts; pts = g_list_next(pts))
                    {
                        dt_masks_point_group_t *pt = pts->data;
                        if (pt->formid == form->formid)
                        {
                            if (m == module)
                            {
                                nbuse = -1;
                                break;
                            }
                            if (nbuse == 0)
                                g_strlcat(str, " (", sizeof(str));
                            g_strlcat(str, " ", sizeof(str));
                            gchar *module_label = dt_history_item_get_name(m);
                            g_strlcat(str, module_label, sizeof(str));
                            g_free(module_label);
                            nbuse++;
                        }
                    }
                }
            }
            if (nbuse != -1)
            {
                if (nbuse > 0)
                    g_strlcat(str, " )", sizeof(str));

                // we add the menu entry
                item = gtk_menu_item_new_with_label(str);
                _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_ADD_EXISTING, form->formid);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu0), item);
                has_unused_shapes = TRUE;
            }
        }

        if (has_unused_shapes)
        {
            item = gtk_menu_item_new_with_label(_("add existing shape"));
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu0);
            gtk_menu_shell_append(menu, item);
        }
    }

    if (!from_group && nb > 0)
    {
        dt_masks_form_t *selected_group = dt_masks_get_from_id(darktable.develop, grpid);
        if (!(selected_group && (selected_group->type & DT_MASKS_GROUP)))
        {
            if (nb == 1)
            {
                item = gtk_menu_item_new_with_label(_("duplicate this shape"));
                _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_DUPLICATE, INVALID_MASKID);
                gtk_menu_shell_append(menu, item);
            }
            item = gtk_menu_item_new_with_label(_("delete this shape"));
            _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_DELETE, INVALID_MASKID);
            gtk_menu_shell_append(menu, item);
        }
        else
        {
            item = gtk_menu_item_new_with_label(_("delete group"));
            _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_DELETE, INVALID_MASKID);
            gtk_menu_shell_append(menu, item);
        }
    }
    else if (nb > 0 && depth < 3)
    {
        item = gtk_menu_item_new_with_label(_("remove from group"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_DELETE, INVALID_MASKID);
        gtk_menu_shell_append(menu, item);
    }

    if (nb > 1 && !from_group)
    {
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        item = gtk_menu_item_new_with_label(_("group the forms"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_GROUP, INVALID_MASKID);
        gtk_menu_shell_append(menu, item);
    }

    if (from_group && depth < 3)
    {
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        _add_tree_operation(menu, self, _("use inverted shape"), DT_MASKS_STATE_INVERSE,
                            selected_states, TRUE);

        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        _add_tree_operation(menu, self, _("mode: union"), DT_MASKS_STATE_UNION, selected_states,
                            !is_last_row);
        _add_tree_operation(menu, self, _("mode: intersection"), DT_MASKS_STATE_INTERSECTION,
                            selected_states, !is_last_row);
        _add_tree_operation(menu, self, _("mode: difference"), DT_MASKS_STATE_DIFFERENCE,
                            selected_states, !is_last_row);
        _add_tree_operation(menu, self, _("mode: sum"), DT_MASKS_STATE_SUM, selected_states,
                            !is_last_row);
        _add_tree_operation(menu, self, _("mode: exclusion"), DT_MASKS_STATE_EXCLUSION,
                            selected_states, !is_last_row);

        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        item = gtk_menu_item_new_with_label(_("move up"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_MOVE_UP, INVALID_MASKID);
        gtk_widget_set_sensitive(item, !is_first_row);
        gtk_menu_shell_append(menu, item);

        item = gtk_menu_item_new_with_label(_("move down"));
        _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_MOVE_DOWN, INVALID_MASKID);
        gtk_widget_set_sensitive(item, !is_last_row);
        gtk_menu_shell_append(menu, item);
    }

    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("delete unused shapes"));
    _bind_masks_tree_item(self, item, DT_MASKS_TREE_ACTION_CLEANUP, INVALID_MASKID);
    gtk_menu_shell_append(menu, item);

    dt_gui_menu_popup(GTK_MENU(menu), treeview, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
    return TRUE;
}

static gboolean _tree_button_pressed(GtkWidget *treeview, GdkEventButton *event,
                                     dt_lib_module_t *self)
{
    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
    GtkTreePath *mouse_path = NULL;
    const gboolean on_row = gtk_tree_view_get_path_at_pos(
        GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &mouse_path, NULL, NULL, NULL);

    if (event->button == GDK_BUTTON_PRIMARY)
    {
        if (!on_row)
            gtk_tree_selection_unselect_all(selection);
        if (mouse_path)
            gtk_tree_path_free(mouse_path);
        return FALSE;
    }

    if (event->button != GDK_BUTTON_SECONDARY)
    {
        if (mouse_path)
            gtk_tree_path_free(mouse_path);
        return FALSE;
    }

    if (on_row && !gtk_tree_selection_path_is_selected(selection, mouse_path))
    {
        if (!dt_modifier_is(event->state, GDK_CONTROL_MASK))
            gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, mouse_path);
    }
    if (mouse_path)
        gtk_tree_path_free(mouse_path);

    return _show_tree_context_menu(treeview, self);
}

static gboolean _tree_context_menu_provider(GtkWidget *treeview, const GdkEventButton *event,
                                            gpointer user_data)
{
    if (event)
        return _tree_button_pressed(treeview, (GdkEventButton *)event, user_data);

    return _show_tree_context_menu(treeview, user_data);
}

static gboolean _tree_restrict_select(GtkTreeSelection *selection, GtkTreeModel *model,
                                      GtkTreePath *path, const gboolean path_currently_selected,
                                      gpointer data)
{
    DT_GUARD_GUI_UPDATE(TRUE);

    // if the change is SELECT->UNSELECT no pb
    if (path_currently_selected)
        return TRUE;

    // if selection is empty, no pb
    if (gtk_tree_selection_count_selected_rows(selection) == 0)
        return TRUE;

    // now we unselect all members of selection with not the same parent node
    // idem for all those with a different depth
    int *indices = gtk_tree_path_get_indices(path);
    const int depth = gtk_tree_path_get_depth(path);

    GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
    GList *items_iter = items;
    while (items_iter)
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        const int dd = gtk_tree_path_get_depth(item);
        int *ii = gtk_tree_path_get_indices(item);
        int ok = 1;
        if (dd != depth)
            ok = 0;
        else if (dd == 1)
            ok = 1;
        else if (ii[dd - 2] != indices[dd - 2])
            ok = 0;
        if (!ok)
        {
            gtk_tree_selection_unselect_path(selection, item);
            g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
            items_iter = items = gtk_tree_selection_get_selected_rows(selection, NULL);
            continue;
        }
        items_iter = g_list_next(items_iter);
    }
    g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
    return TRUE;
}

static gboolean _tree_query_tooltip(GtkWidget *widget, gint x, gint y, const gboolean keyboard_tip,
                                    GtkTooltip *tooltip, gpointer data)
{
    GtkTreeIter iter;
    GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreePath *path = NULL;
    gchar *tmp = NULL;
    gboolean show = FALSE;

    if (!gtk_tree_view_get_tooltip_context(tree_view, &x, &y, keyboard_tip, &model, &path, &iter))
        return FALSE;

    gtk_tree_model_get(model, &iter, TREE_IC_USED_VISIBLE, &show, TREE_USED_TEXT, &tmp, -1);
    if (show)
    {
        gtk_tooltip_set_markup(tooltip, tmp);
        gtk_tree_view_set_tooltip_row(tree_view, tooltip, path);
    }

    gtk_tree_path_free(path);
    g_free(tmp);

    return show;
}

static void _is_form_used(const dt_mask_id_t formid, dt_masks_form_t *grp, char *text,
                          const size_t text_length, int *nb)
{
    if (!grp)
    {
        for (const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
        {
            dt_masks_form_t *form = forms->data;
            if (form->type & DT_MASKS_GROUP)
                _is_form_used(formid, form, text, text_length, nb);
        }
    }
    else if (grp->type & DT_MASKS_GROUP)
    {
        for (const GList *points = grp->points; points; points = g_list_next(points))
        {
            dt_masks_point_group_t *point = points->data;
            dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, point->formid);
            if (form)
            {
                if (point->formid == formid)
                {
                    (*nb)++;
                    if (*nb > 1)
                        g_strlcat(text, "\n", text_length);
                    g_strlcat(text, grp->name, text_length);
                }

                if (form->type & DT_MASKS_GROUP)
                    _is_form_used(formid, form, text, text_length, nb);
            }
        }
    }
}

static void _lib_masks_list_recurs(GtkTreeStore *treestore, GtkTreeIter *toplevel,
                                   dt_masks_form_t *form, const int grp_id, dt_iop_module_t *module,
                                   const int gstate, const float opacity, dt_lib_masks_t *lm)
{
    if (form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        return;
    // we create the text entry
    char str[256] = "";
    g_strlcat(str, form->name, sizeof(str));
    // we get the right pixbufs
    GdkPixbuf *icop = NULL;
    GdkPixbuf *icinv = NULL;
    GdkPixbuf *icuse = NULL;

    const gboolean show = gstate & DT_MASKS_STATE_SHOW;

    if (gstate & DT_MASKS_STATE_UNION)
        icop = lm->ic_union;
    else if (gstate & DT_MASKS_STATE_INTERSECTION)
        icop = lm->ic_intersection;
    else if (gstate & DT_MASKS_STATE_DIFFERENCE)
        icop = lm->ic_difference;
    else if (gstate & DT_MASKS_STATE_SUM)
        icop = lm->ic_sum;
    else if (gstate & DT_MASKS_STATE_EXCLUSION)
        icop = lm->ic_exclusion;

    if (gstate & DT_MASKS_STATE_INVERSE)
        icinv = lm->ic_inverse;

    char str2[1000] = "";
    int nbuse = 0;
    if (grp_id == 0)
    {
        _is_form_used(form->formid, NULL, str2, sizeof(str2), &nbuse);
        if (nbuse > 0)
            icuse = lm->ic_used;
    }

    if (!(form->type & DT_MASKS_GROUP))
    {
        // we just add it to the tree
        GtkTreeIter child;

        if (toplevel)
        {
            // we are within a group
            gtk_tree_store_prepend(treestore, &child, toplevel);
        }
        else
        {
            // skip all groups first
            GtkTreeModel *model = GTK_TREE_MODEL(treestore);
            int pos = 0;
            GtkTreeIter iter;

            if (gtk_tree_model_get_iter_first(model, &iter))
            {
                do
                {
                    if (gtk_tree_model_iter_has_child(model, &iter))
                        ++pos;
                } while (gtk_tree_model_iter_next(model, &iter));
            }

            // insert the child immediately after the last group
            gtk_tree_store_insert(treestore, &child, NULL, pos);
        }

        gtk_tree_store_set(treestore, &child, TREE_TEXT, str, TREE_MODULE, module, TREE_GROUPID,
                           grp_id, TREE_FORMID, form->formid, TREE_EDITABLE, (grp_id == 0),
                           TREE_IC_OP, icop, TREE_IC_OP_VISIBLE, (icop != NULL) && show,
                           TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE, (icinv != NULL),
                           TREE_IC_USED, icuse, TREE_IC_USED_VISIBLE, (nbuse > 0), TREE_USED_TEXT,
                           str2, -1);
        _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child);
    }
    else
    {
        // we first check if it's a "module" group or not
        if (grp_id == 0 && !module)
        {
            for (const GList *iops = darktable.develop->iop; iops; iops = g_list_next(iops))
            {
                dt_iop_module_t *iop = iops->data;
                if ((iop->flags() & IOP_FLAGS_SUPPORTS_BLENDING) &&
                    !(iop->flags() & IOP_FLAGS_NO_MASKS) &&
                    iop->blend_params->mask_id == form->formid)
                {
                    module = iop;
                    break;
                }
            }
        }

        // we add the group node to the tree
        GtkTreeIter child;
        gtk_tree_store_prepend(treestore, &child, toplevel);
        gtk_tree_store_set(treestore, &child, TREE_TEXT, str, TREE_MODULE, module, TREE_GROUPID,
                           grp_id, TREE_FORMID, form->formid, TREE_EDITABLE, (grp_id == 0),
                           TREE_IC_OP, icop, TREE_IC_OP_VISIBLE, (icop != NULL) && show,
                           TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE, (icinv != NULL),
                           TREE_IC_USED, icuse, TREE_IC_USED_VISIBLE, (nbuse > 0), TREE_USED_TEXT,
                           str2, -1);
        _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child);

        // we add all nodes to the tree
        for (const GList *forms = form->points; forms; forms = g_list_next(forms))
        {
            dt_masks_point_group_t *grpt = forms->data;
            dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, grpt->formid);
            if (f)
                _lib_masks_list_recurs(treestore, &child, f, form->formid, module, grpt->state,
                                       grpt->opacity, lm);
        }
    }
}

gboolean _find_mask_iter_by_values(GtkTreeModel *model, GtkTreeIter *iter,
                                   const dt_iop_module_t *module, const dt_mask_id_t formid,
                                   const int level)
{
    do
    {
        dt_mask_id_t fid = INVALID_MASKID;
        dt_iop_module_t *mod;
        _lib_masks_get_values(model, iter, &mod, NULL, &fid);
        gboolean found =
            (fid == formid) &&
            ((level == 1) || (module == NULL || (mod && dt_iop_module_is(module, mod->op))));
        if (found)
            return found;

        GtkTreeIter child, parent = *iter;
        if (gtk_tree_model_iter_children(model, &child, &parent))
        {
            found = _find_mask_iter_by_values(model, &child, module, formid, level + 1);
            if (found)
            {
                *iter = child;
                return found;
            }
        }
    } while (gtk_tree_model_iter_next(model, iter));

    return FALSE;
}

GList *_lib_masks_get_selected(dt_lib_module_t *self)
{
    GList *res = NULL;
    dt_lib_masks_t *lm = self->data;

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

    GList *items = gtk_tree_selection_get_selected_rows(selection, &model);

    for (GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
    {
        GtkTreePath *item = (GtkTreePath *)items_iter->data;
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, item))
        {
            dt_mask_id_t fid = INVALID_MASKID;
            dt_mask_id_t gid = INVALID_MASKID;
            dt_iop_module_t *mod;
            _lib_masks_get_values(model, &iter, &mod, &gid, &fid);
            res = g_list_prepend(res, GINT_TO_POINTER(fid));
            res = g_list_prepend(res, GINT_TO_POINTER(gid));
            res = g_list_prepend(res, (void *)(mod));
        }
    }

    g_list_foreach(items, (GFunc)gtk_tree_path_free, NULL);
    g_list_free(items);

    return res;
}

void gui_update(dt_lib_module_t *self)
{
    /* first destroy all buttons in list */
    dt_lib_masks_t *lm = self->data;
    if (!lm)
        return;

    DT_TRY_GUI_UPDATE();

    // if a treeview is already present, let's get the currently selected items
    // as we are going to recreate the tree.
    GList *selectids = NULL;

    if (lm->treeview)
    {
        selectids = _lib_masks_get_selected(self);
    }

    _lib_masks_inactivate_icons(self);

    GtkTreeStore *treestore;
    // we store : text ; *module ; groupid ; formid
    treestore =
        gtk_tree_store_new(TREE_COUNT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT, G_TYPE_INT,
                           G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                           G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, G_TYPE_STRING);

    // we first add all groups
    for (const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
    {
        dt_masks_form_t *form = forms->data;
        if (form->type & DT_MASKS_GROUP)
            _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm);
    }

    // and we add all forms
    for (const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
    {
        dt_masks_form_t *form = forms->data;
        if (!(form->type & DT_MASKS_GROUP))
            _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm);
    }

    gtk_tree_view_set_model(GTK_TREE_VIEW(lm->treeview), GTK_TREE_MODEL(treestore));

    // select the images as selected in the previous tree
    if (selectids)
    {
        GList *ids = selectids;
        while (ids)
        {
            GtkTreeModel *model = GTK_TREE_MODEL(treestore);
            dt_iop_module_t *mod = ids->data;
            ids = g_list_next(ids);
            // const int gid = GPOINTER_TO_INT(ids->data); // not needed, skip it
            ids = g_list_next(ids);
            const int fid = GPOINTER_TO_INT(ids->data);
            ids = g_list_next(ids);

            GtkTreeIter iter;
            // get formid in group for the given module
            const gboolean found = gtk_tree_model_get_iter_first(model, &iter) &&
                                   _find_mask_iter_by_values(model, &iter, mod, fid, 1);

            if (found)
            {
                GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
                gtk_tree_view_expand_to_path(GTK_TREE_VIEW(lm->treeview), path);
                gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(lm->treeview), path, NULL, TRUE, 0.5,
                                             0.5);
                gtk_tree_path_free(path);
                GtkTreeSelection *selection =
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
                gtk_tree_selection_select_iter(selection, &iter);
            }
        }
        g_list_free(selectids);
    }

    g_object_unref(treestore);

    DT_LEAVE_GUI_UPDATE();

    dt_gui_widget_reallocate_now(lm->treeview);
}

static void _lib_masks_recreate_list(dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;
    dt_lib_gui_queue_update(self);

    DT_TRY_GUI_UPDATE();

    _update_all_properties(lm);

    DT_LEAVE_GUI_UPDATE();
}

static gboolean _update_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                gpointer data)
{
    if (!iter)
        return 0;

    // we retrieve the ids
    dt_mask_id_t grid = INVALID_MASKID;
    dt_mask_id_t id = INVALID_MASKID;
    _lib_masks_get_values(model, iter, NULL, &grid, &id);

    // we retrieve the forms
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
    if (!form)
        return 0;
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);

    // and the values
    int state = 0;
    float opacity = 1.0f;

    if (grp && (grp->type & DT_MASKS_GROUP))
    {
        for (const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
            dt_masks_point_group_t *pt = pts->data;
            if (pt->formid == id)
            {
                state = pt->state;
                opacity = pt->opacity;
                break;
            }
        }
    }

    _set_iter_name(data, form, state, opacity, model, iter);
    return 0;
}

static void _lib_masks_update_list(dt_lib_module_t *self)
{
    dt_lib_masks_t *lm = self->data;
    // for each node , we refresh the string
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    gtk_tree_model_foreach(model, _update_foreach, lm);
}

static gboolean _remove_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                gpointer data)
{
    if (!iter)
        return 0;
    GList **rl = (GList **)data;
    const dt_mask_id_t refid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "formid"));
    const dt_mask_id_t refgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "groupid"));

    dt_mask_id_t grid = INVALID_MASKID;
    dt_mask_id_t id = INVALID_MASKID;
    _lib_masks_get_values(model, iter, NULL, &grid, &id);

    if (grid == refgid && id == refid)
    {
        GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
        *rl = g_list_append(*rl, rowref);
    }
    return 0;
}

static void _lib_masks_remove_item(dt_lib_module_t *self, const dt_mask_id_t formid,
                                   const dt_mask_id_t parentid)
{
    dt_lib_masks_t *lm = self->data;
    // for each node , we refresh the string
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    GList *rl = NULL;
    g_object_set_data(G_OBJECT(model), "formid", GUINT_TO_POINTER(formid));
    g_object_set_data(G_OBJECT(model), "groupid", GUINT_TO_POINTER(parentid));
    gtk_tree_model_foreach(model, _remove_foreach, &rl);

    for (const GList *rlt = rl; rlt; rlt = g_list_next(rlt))
    {
        GtkTreeRowReference *rowref = (GtkTreeRowReference *)rlt->data;
        GtkTreePath *path = gtk_tree_row_reference_get_path(rowref);
        gtk_tree_row_reference_free(rowref);
        if (path)
        {
            GtkTreeIter iter;
            if (gtk_tree_model_get_iter(model, &iter, path))
            {
                gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
            }
            gtk_tree_path_free(path);
        }
    }
    g_list_free(rl);
}

static gboolean _lib_masks_selection_change_r(GtkTreeModel *model, GtkTreeSelection *selection,
                                              GtkTreeIter *iter, struct dt_iop_module_t *module,
                                              const dt_mask_id_t selectid, const int level)
{
    gboolean found = FALSE;

    GtkTreeIter i = *iter;
    do
    {
        dt_mask_id_t id = INVALID_MASKID;
        dt_iop_module_t *mod;
        _lib_masks_get_values(model, &i, &mod, NULL, &id);

        if ((id == selectid) &&
            ((level == 1) || (module == NULL || (mod && dt_iop_module_is(module, mod->op)))))
        {
            gtk_tree_selection_select_iter(selection, &i);
            found = TRUE;
            break;
        }

        // check for children if any
        GtkTreeIter child, parent = i;
        if (gtk_tree_model_iter_children(model, &child, &parent))
        {
            found = _lib_masks_selection_change_r(model, selection, &child, module, selectid,
                                                  level + 1);
            if (found)
            {
                break;
            }
        }
    } while (gtk_tree_model_iter_next(model, &i) == TRUE);

    return found;
}

static void _lib_masks_selection_change(dt_lib_module_t *self, struct dt_iop_module_t *module,
                                        const dt_mask_id_t selectid)
{
    dt_lib_masks_t *lm = self->data;
    if (!lm->treeview)
        return;

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
    if (!model)
        return;

    DT_ENTER_GUI_UPDATE();

    // we first unselect all
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
    gtk_tree_selection_unselect_all(selection);

    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

    // we go through all nodes
    if (valid)
    {
        gtk_tree_view_expand_all(GTK_TREE_VIEW(lm->treeview));
        const gboolean found =
            _lib_masks_selection_change_r(model, selection, &iter, module, selectid, 1);
        if (!found)
            gtk_tree_view_collapse_all(GTK_TREE_VIEW(lm->treeview));
    }

    DT_LEAVE_GUI_UPDATE();

    _update_all_properties(lm);
}

static GdkPixbuf *_get_pixbuf_from_cairo(DTGTKCairoPaintIconFunc paint, const int width,
                                         const int height)
{
    cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(cst);
    dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BUTTON_FG, 1.0);
    paint(cr, 0, 0, width, height, 0, NULL);
    cairo_destroy(cr);
    guchar *data = cairo_image_surface_get_data(cst);
    dt_draw_cairo_to_gdk_pixbuf(data, width, height);
    return gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8, width, height,
                                    cairo_image_surface_get_stride(cst), NULL, NULL);
}

void gui_init(dt_lib_module_t *self)
{
    /* initialize ui widgets */
    dt_lib_masks_t *d = g_malloc0(sizeof(dt_lib_masks_t));
    self->data = (void *)d;

    // initialise all masks pixbuf. This is needed for the "automatic"
    // cell renderer of the treeview
    const int bs2 = DT_PIXEL_APPLY_DPI(13);
    d->ic_inverse = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_inverse, bs2, bs2);
    d->ic_used = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_used, bs2, bs2);
    d->ic_union = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_union, bs2 * 2, bs2);
    d->ic_intersection = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_intersection, bs2 * 2, bs2);
    d->ic_difference = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_difference, bs2 * 2, bs2);
    d->ic_sum = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_sum, bs2 * 2, bs2);
    d->ic_exclusion = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_exclusion, bs2 * 2, bs2);

    // initialise widgets
    d->bt_gradient = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_gradient, 0, NULL);
    dt_action_define(DT_ACTION(self), N_("shapes"), N_("add gradient"), d->bt_gradient,
                     &dt_action_def_toggle);
    g_signal_connect(G_OBJECT(d->bt_gradient), "button-press-event", G_CALLBACK(_bt_add_shape),
                     GINT_TO_POINTER(DT_MASKS_GRADIENT));
    gtk_widget_set_tooltip_text(d->bt_gradient, _("add gradient"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_gradient), FALSE);

    d->bt_path = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_path, 0, NULL);
    dt_action_define(DT_ACTION(self), N_("shapes"), N_("add path"), d->bt_path,
                     &dt_action_def_toggle);
    g_signal_connect(G_OBJECT(d->bt_path), "button-press-event", G_CALLBACK(_bt_add_shape),
                     GINT_TO_POINTER(DT_MASKS_PATH));
    gtk_widget_set_tooltip_text(d->bt_path, _("add path"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_path), FALSE);

    d->bt_ellipse = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_ellipse, 0, NULL);
    dt_action_define(DT_ACTION(self), N_("shapes"), N_("add ellipse"), d->bt_ellipse,
                     &dt_action_def_toggle);
    g_signal_connect(G_OBJECT(d->bt_ellipse), "button-press-event", G_CALLBACK(_bt_add_shape),
                     GINT_TO_POINTER(DT_MASKS_ELLIPSE));
    gtk_widget_set_tooltip_text(d->bt_ellipse, _("add ellipse"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_ellipse), FALSE);

    d->bt_circle = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_circle, 0, NULL);
    dt_action_define(DT_ACTION(self), N_("shapes"), N_("add circle"), d->bt_circle,
                     &dt_action_def_toggle);
    g_signal_connect(G_OBJECT(d->bt_circle), "button-press-event", G_CALLBACK(_bt_add_shape),
                     GINT_TO_POINTER(DT_MASKS_CIRCLE));
    gtk_widget_set_tooltip_text(d->bt_circle, _("add circle"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_circle), FALSE);

    d->bt_brush = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_brush, 0, NULL);
    dt_action_define(DT_ACTION(self), N_("shapes"), N_("add brush"), d->bt_brush,
                     &dt_action_def_toggle);
    g_signal_connect(G_OBJECT(d->bt_brush), "button-press-event", G_CALLBACK(_bt_add_shape),
                     GINT_TO_POINTER(DT_MASKS_BRUSH));
    gtk_widget_set_tooltip_text(d->bt_brush, _("add brush"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_brush), FALSE);

    d->treeview = gtk_tree_view_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "shapes");
    gtk_tree_view_append_column(GTK_TREE_VIEW(d->treeview), col);

    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_OP, NULL);
    gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_OP_VISIBLE);
    renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_INVERSE, NULL);
    gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_INVERSE_VISIBLE);
    renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_add_attribute(col, renderer, "text", TREE_TEXT);
    gtk_tree_view_column_add_attribute(col, renderer, "editable", TREE_EDITABLE);
    g_signal_connect(renderer, "edited", G_CALLBACK(_tree_cell_edited), self);
    dt_gui_commit_on_focus_loss(renderer, NULL);
    renderer = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_end(col, renderer, FALSE);
    gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_USED, NULL);
    gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_USED_VISIBLE);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_set_select_function(selection, _tree_restrict_select, d, NULL);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->treeview), FALSE);
    gtk_widget_set_has_tooltip(d->treeview, TRUE);
    g_signal_connect(d->treeview, "query-tooltip", G_CALLBACK(_tree_query_tooltip), NULL);
    g_signal_connect(selection, "changed", G_CALLBACK(_tree_selection_change), d);
    g_signal_connect(d->treeview, "button-press-event", G_CALLBACK(_tree_button_pressed), self);
    g_object_set_data(G_OBJECT(d->treeview), "lib-instance", self);
    d->tree_action = dt_action_define(DT_ACTION(self), N_("shapes"), N_("selected shapes"),
                                      d->treeview, &_action_def_masks_tree);
    dt_action_set_context_menu_provider_only(d->tree_action, TRUE);
    dt_gui_context_menu_attach_provider(d->treeview, _tree_context_menu_provider, self, NULL);

    GtkWidget *shape_buttons =
        dt_gui_hbox(dt_gui_expand(dt_ui_label_new(_("created shapes"))), d->bt_brush, d->bt_circle,
                    d->bt_ellipse, d->bt_path, d->bt_gradient);
    self->widget = dt_gui_vbox(
        shape_buttons, dt_ui_resize_wrap(d->treeview, 200, "plugins/darkroom/masks/heightview"));

    dt_gui_new_collapsible_section(&d->cs, "plugins/darkroom/masks/expand_properties",
                                   _("properties"), GTK_BOX(self->widget), DT_ACTION(self));
    d->none_label = dt_ui_label_new(_("no shapes selected"));
    dt_gui_box_add(d->cs.container, d->none_label);
    gtk_widget_show_all(GTK_WIDGET(d->cs.container));
    gtk_widget_set_no_show_all(GTK_WIDGET(d->cs.container), TRUE);

    for (int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
    {
        GtkWidget *w;
        if (_masks_properties[i].boolean)
        {
            w = gtk_check_button_new_with_label(_(_masks_properties[i].name));
            dt_action_define(DT_ACTION(self), N_("properties"), _masks_properties[i].name, w,
                             &dt_action_def_toggle);
            d->last_value[i] = (float)gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
            g_signal_connect(G_OBJECT(w), "toggled", G_CALLBACK(_property_changed),
                             GINT_TO_POINTER(i));
        }
        else
        {
            w = dt_bauhaus_slider_new_action(DT_ACTION(self), _masks_properties[i].min,
                                             _masks_properties[i].max, 0, 0.0, 2);
            dt_bauhaus_widget_set_label(w, N_("properties"), _masks_properties[i].name);
            dt_bauhaus_slider_set_format(w, _masks_properties[i].format);
            dt_bauhaus_slider_set_digits(w, 2);
            if (_masks_properties[i].relative)
                dt_bauhaus_slider_set_log_curve(w);
            d->last_value[i] = dt_bauhaus_slider_get(w);
            g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_property_changed),
                             GINT_TO_POINTER(i));
        }
        d->property[i] = w;
        dt_gui_box_add(d->cs.container, w);
    }

    d->pressure = dt_gui_preferences_enum(DT_ACTION(self), "pressure_sensitivity");
    dt_bauhaus_widget_set_label(d->pressure, N_("properties"), N_("pressure"));
    d->smoothing = dt_gui_preferences_enum(DT_ACTION(self), "brush_smoothing");
    dt_bauhaus_widget_set_label(d->smoothing, N_("properties"), N_("smoothing"));
    dt_gui_box_add(d->cs.container, d->pressure, d->smoothing);

    // set proxy functions
    darktable.develop->proxy.masks.module = self;
    darktable.develop->proxy.masks.list_change = _lib_masks_recreate_list;
    darktable.develop->proxy.masks.list_update = _lib_masks_update_list;
    darktable.develop->proxy.masks.list_remove = _lib_masks_remove_item;
    darktable.develop->proxy.masks.selection_change = _lib_masks_selection_change;
}

void gui_cleanup(dt_lib_module_t *self)
{
    g_free(self->data);
    self->data = NULL;
}
