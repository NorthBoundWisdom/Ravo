/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "gui/context_menu.h"

#include "bauhaus/bauhaus.h"
#include "common/act_on.h"
#include "common/selection.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "views/view.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <string.h>

typedef struct dt_context_menu_action_t
{
    dt_action_t *action;
    int instance;
    dt_action_element_t element;
    dt_action_effect_t effect;
    GList *images;
    dt_imgid_t main_image;
    gpointer payload;
    GDestroyNotify destroy_payload;
} dt_context_menu_action_t;

typedef struct dt_context_menu_image_provider_t
{
    dt_gui_context_menu_image_id_callback_t image_id;
    gpointer user_data;
} dt_context_menu_image_provider_t;

typedef struct dt_context_menu_widget_provider_t
{
    dt_gui_context_menu_widget_provider_t provider;
    gpointer user_data;
    GDestroyNotify destroy_user_data;
} dt_context_menu_widget_provider_t;

static GQuark _context_menu_quark = 0;
static GQuark _context_menu_image_quark = 0;
static GQuark _context_menu_widget_provider_quark = 0;
static dt_context_menu_action_t *_active_menu_action = NULL;

static void _free_widget_provider(gpointer data)
{
    dt_context_menu_widget_provider_t *provider = data;
    if (!provider)
        return;

    if (provider->destroy_user_data)
        provider->destroy_user_data(provider->user_data);
    g_free(provider);
}

static void _free_menu_action(gpointer data)
{
    dt_context_menu_action_t *item = data;
    if (!item)
        return;

    g_list_free(item->images);
    if (item->destroy_payload)
        item->destroy_payload(item->payload);
    g_free(item);
}

static dt_context_menu_action_t *_new_menu_action_context(dt_action_t *action, const int instance,
                                                           const dt_action_element_t element,
                                                           const dt_action_effect_t effect,
                                                           const GList *images,
                                                           const dt_imgid_t main_image,
                                                           gpointer payload,
                                                           GDestroyNotify destroy_payload)
{
    dt_context_menu_action_t *context = g_malloc0(sizeof(*context));
    context->action = action;
    context->instance = instance;
    context->element = element;
    context->effect = effect;
    context->images = g_list_copy((GList *)images);
    context->main_image = main_image;
    context->payload = payload;
    context->destroy_payload = destroy_payload;
    return context;
}

gpointer dt_gui_context_menu_get_action_payload(const dt_action_t *action)
{
    return _active_menu_action && _active_menu_action->action == action ?
               _active_menu_action->payload :
               NULL;
}

static gboolean _is_destructive(const dt_action_t *action)
{
    return action && action->id &&
           (strstr(action->id, "delete") || strstr(action->id, "remove") ||
            strstr(action->id, "discard") || strstr(action->id, "clear"));
}

static gboolean _is_context_excluded(const dt_action_t *action)
{
    if (!action || !action->id)
        return FALSE;

    return action->context_menu_provider_only || !g_strcmp0(action->id, "quit") ||
           !g_strcmp0(action->id, "show accels window") ||
           !g_strcmp0(action->id, "reinitialise input devices") ||
           !g_strcmp0(action->id, "modifiers");
}

static gboolean _effect_is_discrete(const dt_action_element_def_t *element,
                                    const dt_action_effect_t effect)
{
    if (!element || effect < 0)
        return FALSE;

    if (element->effects == dt_action_effect_value)
        return effect == DT_ACTION_EFFECT_RESET || effect == DT_ACTION_EFFECT_TOP ||
               effect == DT_ACTION_EFFECT_BOTTOM;

    if (element->effects == dt_action_effect_selection)
        return effect == DT_ACTION_EFFECT_RESET || effect > DT_ACTION_EFFECT_COMBO_SEPARATOR;

    if (element->effects == dt_action_effect_toggle ||
        element->effects == dt_action_effect_activate)
        return effect == DT_ACTION_EFFECT_DEFAULT_KEY;

    return TRUE;
}

static int _projected_effect_count(dt_action_t *action, const dt_action_element_t element)
{
    const dt_action_element_def_t *elements = dt_action_get_elements(action);
    if (!elements || !elements[element].effects)
        return 0;

    const dt_action_element_def_t *definition = &elements[element];
    int count = definition->effects == dt_action_effect_value;
    const int effects = dt_action_get_effect_count(action, element);
    for (int effect = 0; effect < effects; effect++)
        count += _effect_is_discrete(definition, effect);

    return count + dt_action_get_combo_count(action, element);
}

static void _menu_action_activate(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    dt_context_menu_action_t *context = user_data;
    dt_action_status_t status;
    dt_action_get_status(context->action, context->instance, context->element, context->effect,
                         &status);
    if (!status.applicable || !status.enabled)
        return;

    dt_act_on_context_t *images = NULL;
    if (context->images)
        images = dt_act_on_push_context(context->images, context->main_image);

    dt_context_menu_action_t *previous = _active_menu_action;
    _active_menu_action = context;
    dt_action_invoke(context->action, context->instance, context->element, context->effect, 1.0f);
    _active_menu_action = previous;

    if (images)
        dt_act_on_pop_context(images);
}

static void _menu_slider_set(GtkMenuItem *item, gpointer user_data)
{
    dt_context_menu_action_t *context = user_data;
    dt_action_status_t status;
    dt_action_get_status(context->action, context->instance, context->element,
                         DT_ACTION_EFFECT_SET, &status);
    if (!status.applicable || !status.enabled)
        return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        _("set value"), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), GTK_DIALOG_MODAL,
        _("cancel"), GTK_RESPONSE_CANCEL, _("set"), GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *entry = gtk_entry_new();
    gchar *value = g_strdup_printf("%g", status.value);
    gtk_entry_set_text(GTK_ENTRY(entry), value);
    g_free(value);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), entry);
    gtk_widget_show_all(dialog);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    {
        gchar *end = NULL;
        const double set_value = g_ascii_strtod(gtk_entry_get_text(GTK_ENTRY(entry)), &end);
        if (end && *end == '\0' && isfinite(set_value))
        {
            dt_act_on_context_t *images = NULL;
            if (context->images)
                images = dt_act_on_push_context(context->images, context->main_image);
            dt_action_invoke(context->action, context->instance, context->element,
                             DT_ACTION_EFFECT_SET, set_value);
            if (images)
                dt_act_on_pop_context(images);
        }
    }

    gtk_widget_destroy(dialog);
    (void)item;
}

static GtkWidget *_new_action_item(const gchar *label, dt_action_t *action, const int instance,
                                   const dt_action_element_t element,
                                   const dt_action_effect_t effect, const GList *images,
                                   const dt_imgid_t main_image, GSList **radio_group,
                                   gpointer payload, GDestroyNotify destroy_payload)
{
    dt_action_status_t status;
    dt_action_get_status(action, instance, element, effect, &status);

    const dt_action_element_def_t *elements = dt_action_get_elements(action);
    const gboolean check_item = elements && elements[element].effects == dt_action_effect_toggle;
    const gboolean radio_item = elements && elements[element].effects == dt_action_effect_selection &&
                                effect > DT_ACTION_EFFECT_COMBO_SEPARATOR;
    GtkWidget *item =
        radio_item ? gtk_radio_menu_item_new_with_label(radio_group ? *radio_group : NULL, label) :
                     (check_item ? gtk_check_menu_item_new_with_label(label) :
                                   gtk_menu_item_new_with_label(label));
    if (radio_item && radio_group)
        *radio_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
    if (check_item || radio_item)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), status.checked);

    gchar *shortcut = dt_action_get_shortcut_label(action, instance, element, effect);
    if (shortcut && *shortcut)
    {
        gchar *with_shortcut = g_strdup_printf("%s\t%s", label, shortcut);
        gtk_menu_item_set_label(GTK_MENU_ITEM(item), with_shortcut);
        g_free(with_shortcut);
    }
    g_free(shortcut);

    gtk_widget_set_sensitive(item, status.applicable && status.enabled);
    if (status.reason)
        gtk_widget_set_tooltip_text(item, status.reason);

    dt_context_menu_action_t *context =
        _new_menu_action_context(action, instance, element, effect, images, main_image, payload,
                                 destroy_payload);
    g_object_set_data_full(G_OBJECT(item), "dt-context-menu-action", context, _free_menu_action);
    g_signal_connect(item, "activate", G_CALLBACK(_menu_action_activate), context);

    return item;
}

GtkWidget *dt_gui_context_menu_action_item_new(const gchar *label, dt_action_t *action,
                                               const int instance,
                                               const dt_action_element_t element,
                                               const dt_action_effect_t effect, gpointer payload,
                                               GDestroyNotify destroy_payload)
{
    return _new_action_item(label, action, instance, element, effect, NULL, NO_IMGID, NULL,
                            payload, destroy_payload);
}

void dt_gui_context_menu_bind_action_item(GtkMenuItem *item, dt_action_t *action,
                                          const int instance, const dt_action_element_t element,
                                          const dt_action_effect_t effect, gpointer payload,
                                          GDestroyNotify destroy_payload)
{
    if (!item || !action)
    {
        if (destroy_payload)
            destroy_payload(payload);
        return;
    }

    dt_action_status_t status;
    dt_action_get_status(action, instance, element, effect, &status);
    gtk_widget_set_sensitive(GTK_WIDGET(item), status.applicable && status.enabled);
    if (status.reason)
        gtk_widget_set_tooltip_text(GTK_WIDGET(item), status.reason);

    dt_context_menu_action_t *context =
        _new_menu_action_context(action, instance, element, effect, NULL, NO_IMGID, payload,
                                 destroy_payload);
    g_object_set_data_full(G_OBJECT(item), "dt-context-menu-action", context, _free_menu_action);
    g_signal_connect(item, "activate", G_CALLBACK(_menu_action_activate), context);
}

static GtkWidget *_new_slider_set_item(dt_action_t *action, const int instance,
                                       const dt_action_element_t element, const GList *images,
                                       const dt_imgid_t main_image)
{
    dt_action_status_t status;
    dt_action_get_status(action, instance, element, DT_ACTION_EFFECT_SET, &status);
    GtkWidget *item = gtk_menu_item_new_with_label(_("set value..."));
    gtk_widget_set_sensitive(item, status.applicable && status.enabled);
    if (status.reason)
        gtk_widget_set_tooltip_text(item, status.reason);

    dt_context_menu_action_t *context = _new_menu_action_context(
        action, instance, element, DT_ACTION_EFFECT_SET, images, main_image, NULL, NULL);
    g_object_set_data_full(G_OBJECT(item), "dt-context-menu-action", context, _free_menu_action);
    g_signal_connect(item, "activate", G_CALLBACK(_menu_slider_set), context);
    return item;
}

static gboolean _append_effects(GtkMenuShell *shell, dt_action_t *action, const int instance,
                                const dt_action_element_t element, const gchar *fallback_label,
                                const GList *images, const dt_imgid_t main_image)
{
    const dt_action_element_def_t *elements = dt_action_get_elements(action);
    if (!elements || !elements[element].effects)
        return FALSE;

    const dt_action_element_def_t *definition = &elements[element];
    const int effects = dt_action_get_effect_count(action, element);
    const int combos = dt_action_get_combo_count(action, element);
    const int projected = _projected_effect_count(action, element);
    int count = 0;

    if (definition->effects == dt_action_effect_value)
    {
        gtk_menu_shell_append(shell,
                              _new_slider_set_item(action, instance, element, images, main_image));
        count++;
    }

    for (int effect = 0; effect < effects; effect++)
    {
        if (!_effect_is_discrete(definition, effect))
            continue;

        gchar *effect_label = dt_action_get_effect_label(action, element, effect);
        const gchar *label = fallback_label && projected == 1 ? fallback_label : effect_label;
        if (label)
        {
            gtk_menu_shell_append(shell, _new_action_item(label, action, instance, element, effect,
                                                           images, main_image, NULL, NULL, NULL));
            count++;
        }
        g_free(effect_label);
    }

    GSList *radio_group = NULL;
    for (int combo = 0; combo < combos; combo++)
    {
        const dt_action_effect_t effect = DT_ACTION_EFFECT_COMBO_SEPARATOR + 1 + combo;
        gchar *label = dt_action_get_effect_label(action, element, effect);
        if (label)
        {
            gtk_menu_shell_append(shell, _new_action_item(label, action, instance, element, effect,
                                                           images, main_image, &radio_group, NULL,
                                                           NULL));
            count++;
        }
        g_free(label);
    }

    return count > 0;
}

static gboolean _append_actions(GtkMenuShell *shell, const dt_action_t *root, const int instance,
                                const GList *images, const dt_imgid_t main_image,
                                gboolean destructive);

static gboolean _append_action_range(GtkMenuShell *shell, dt_action_t *action, const int instance,
                                     const GList *images, const dt_imgid_t main_image,
                                     const int first_element, const int element_count)
{
    if (!action || action->type == DT_ACTION_TYPE_CATEGORY ||
        action->type == DT_ACTION_TYPE_GLOBAL || action->type == DT_ACTION_TYPE_VIEW ||
        action->type == DT_ACTION_TYPE_SECTION)
        return FALSE;

    if (!dt_action_get_definition(action) && action->type != DT_ACTION_TYPE_COMMAND &&
        action->type != DT_ACTION_TYPE_PRESET)
        return FALSE;

    const dt_action_element_def_t *elements = dt_action_get_elements(action);
    const int elements_count = dt_action_get_element_count(action);
    const int first = CLAMP(first_element, 0, elements_count);
    const int last = element_count < 0 ? elements_count :
                                       MIN(elements_count, first + element_count);
    gchar *label = dt_action_get_full_label(action);
    gboolean appended = FALSE;

    if (!elements_count && first == 0)
    {
        gtk_menu_shell_append(shell, _new_action_item(label, action, instance,
                                                       DT_ACTION_ELEMENT_DEFAULT,
                                                       DT_ACTION_EFFECT_DEFAULT_KEY, images,
                                                       main_image, NULL, NULL, NULL));
        appended = TRUE;
    }
    else if (last - first == 1 && !elements[first].name)
    {
        appended = _append_effects(shell, action, instance, first, label, images, main_image);
    }
    else if (last > first)
    {
        GtkWidget *item = gtk_menu_item_new_with_label(label);
        GtkWidget *submenu = gtk_menu_new();
        int entries = 0;

        for (int element = first; element < last; element++)
        {
            gchar *element_label = elements[element].name ? g_strdup(Q_(elements[element].name)) :
                                                            g_strdup(label);
            if (_projected_effect_count(action, element) == 1)
            {
                if (_append_effects(GTK_MENU_SHELL(submenu), action, instance, element,
                                    element_label, images, main_image))
                    entries++;
            }
            else
            {
                GtkWidget *element_item = gtk_menu_item_new_with_label(element_label);
                GtkWidget *effects_menu = gtk_menu_new();
                if (_append_effects(GTK_MENU_SHELL(effects_menu), action, instance, element, NULL,
                                    images, main_image))
                {
                    gtk_menu_item_set_submenu(GTK_MENU_ITEM(element_item), effects_menu);
                    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), element_item);
                    entries++;
                }
                else
                    gtk_widget_destroy(element_item);
            }
            g_free(element_label);
        }

        if (entries)
        {
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
            gtk_menu_shell_append(shell, item);
            appended = TRUE;
        }
        else
            gtk_widget_destroy(item);
    }

    g_free(label);
    return appended;
}

static gboolean _append_action(GtkMenuShell *shell, dt_action_t *action, const int instance,
                               const GList *images, const dt_imgid_t main_image)
{
    return _append_action_range(shell, action, instance, images, main_image, 0, -1);
}

gboolean dt_gui_context_menu_append_action_elements(GtkMenuShell *shell, dt_action_t *action,
                                                     const int instance, const int first_element,
                                                     const int element_count)
{
    if (!shell || !action || first_element < 0 || element_count <= 0)
        return FALSE;

    return _append_action_range(shell, action, instance, NULL, NO_IMGID, first_element,
                                element_count);
}

static gboolean _append_actions(GtkMenuShell *shell, const dt_action_t *root, const int instance,
                                const GList *images, const dt_imgid_t main_image,
                                const gboolean destructive)
{
    gboolean appended = FALSE;
    for (const dt_action_t *child = dt_action_get_children(root); child; child = child->next)
    {
        if (_is_context_excluded(child))
            continue;

        if (child->type == DT_ACTION_TYPE_CATEGORY || child->type == DT_ACTION_TYPE_GLOBAL ||
            child->type == DT_ACTION_TYPE_VIEW || child->type == DT_ACTION_TYPE_SECTION)
        {
            GtkMenu *submenu = GTK_MENU(gtk_menu_new());
            const gboolean normal = _append_actions(GTK_MENU_SHELL(submenu), child, instance, images,
                                                     main_image, FALSE);
            GtkWidget *separator = normal ? gtk_separator_menu_item_new() : NULL;
            if (separator)
                gtk_menu_shell_append(GTK_MENU_SHELL(submenu), separator);
            const gboolean dangerous = _append_actions(GTK_MENU_SHELL(submenu), child, instance,
                                                        images, main_image, TRUE);
            if (separator && !dangerous)
                gtk_widget_destroy(separator);
            if (normal || dangerous)
            {
                GtkWidget *item = gtk_menu_item_new_with_label(child->label);
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), GTK_WIDGET(submenu));
                gtk_menu_shell_append(shell, item);
                appended = TRUE;
            }
            else
                gtk_widget_destroy(GTK_WIDGET(submenu));
            continue;
        }

        if (_is_destructive(child) != destructive)
            continue;
        appended |= _append_action(shell, (dt_action_t *)child, instance, images, main_image);
    }
    return appended;
}

static GtkMenu *_build_action_menu(dt_action_t *root, const int instance, const GList *images,
                                   const dt_imgid_t main_image)
{
    GtkMenu *menu = GTK_MENU(gtk_menu_new());
    const gboolean normal = _append_actions(GTK_MENU_SHELL(menu), root, instance, images, main_image,
                                             FALSE);
    GtkWidget *separator = normal ? gtk_separator_menu_item_new() : NULL;
    if (separator)
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
    const gboolean destructive = _append_actions(GTK_MENU_SHELL(menu), root, instance, images,
                                                  main_image, TRUE);
    if (separator && !destructive)
        gtk_widget_destroy(separator);
    return menu;
}

static GtkMenu *_build_single_action_menu(dt_action_t *action, const int instance,
                                          const GList *images, const dt_imgid_t main_image)
{
    GtkMenu *menu = GTK_MENU(gtk_menu_new());
    _append_action(GTK_MENU_SHELL(menu), action, instance, images, main_image);
    return menu;
}

static gboolean _menu_has_items(GtkMenu *menu)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    const gboolean has_items = children != NULL;
    g_list_free(children);
    return has_items;
}

static void _append_action_root_submenu(GtkMenuShell *shell, const gchar *label,
                                        dt_action_t *root, const GList *images,
                                        const dt_imgid_t main_image)
{
    GtkMenu *submenu = _build_action_menu(root, 0, images, main_image);
    if (!_menu_has_items(submenu))
    {
        gtk_widget_destroy(GTK_WIDGET(submenu));
        return;
    }

    GtkWidget *item = gtk_menu_item_new_with_label(label);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), GTK_WIDGET(submenu));
    gtk_menu_shell_append(shell, item);
}

static void _append_more_actions(GtkMenuShell *shell, const GList *images,
                                 const dt_imgid_t main_image)
{
    GtkMenu *submenu = GTK_MENU(gtk_menu_new());
    const dt_view_t *view = dt_view_manager_get_current_view(darktable.view_manager);
    if (view)
        _append_action_root_submenu(GTK_MENU_SHELL(submenu), _("current view"),
                                    (dt_action_t *)view, images, main_image);
    _append_action_root_submenu(GTK_MENU_SHELL(submenu), _("global"),
                                &darktable.control->actions_global, images, main_image);

    if (!_menu_has_items(submenu))
    {
        gtk_widget_destroy(GTK_WIDGET(submenu));
        return;
    }

    GtkWidget *item = gtk_menu_item_new_with_label(_("more actions"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), GTK_WIDGET(submenu));
    gtk_menu_shell_append(shell, item);
}

gboolean dt_gui_context_menu_show_for_widget(GtkWidget *widget)
{
    GtkWidget *action_widget = NULL;
    dt_action_t *action = dt_action_find_widget(widget, &action_widget);
    if (!action || action->context_menu_provider_only)
        return FALSE;

    int instance = 0;
    dt_action_get_instance(action, action_widget, &instance);
    GtkMenu *menu = _build_single_action_menu(action, instance, NULL, NO_IMGID);
    const gboolean populated = _menu_has_items(menu);
    if (!populated)
    {
        gtk_widget_destroy(GTK_WIDGET(menu));
        return FALSE;
    }

    dt_gui_menu_popup(menu, action_widget ? action_widget : widget, GDK_GRAVITY_SOUTH_WEST,
                      GDK_GRAVITY_NORTH_WEST);
    return TRUE;
}

static gboolean _prepare_image_context(const dt_imgid_t image, GList **images)
{
    if (!dt_is_valid_imgid(image))
        return FALSE;

    GList *selection = dt_selection_get_list(darktable.selection, FALSE, FALSE);
    if (!g_list_find(selection, GINT_TO_POINTER(image)))
    {
        if (dt_view_get_current() == DT_VIEW_DARKROOM && darktable.develop &&
            dt_is_valid_imgid(darktable.develop->image_storage.id) &&
            darktable.develop->image_storage.id != image)
        {
            // The filmstrip must keep the image currently being edited selected.  The Action
            // invocation below still receives only the right-clicked image snapshot.
            dt_selection_clear(darktable.selection);
            dt_selection_select(darktable.selection, darktable.develop->image_storage.id);
            dt_selection_select(darktable.selection, image);
            g_list_free(selection);
            *images = g_list_append(NULL, GINT_TO_POINTER(image));
            return TRUE;
        }

        dt_selection_select_single(darktable.selection, image);
        g_list_free(selection);
        selection = dt_selection_get_list(darktable.selection, FALSE, FALSE);
    }

    *images = selection;
    return TRUE;
}

gboolean dt_gui_context_menu_show_image(GtkWidget *widget, const dt_imgid_t image)
{
    GList *images = NULL;
    if (!_prepare_image_context(image, &images))
        return FALSE;

    /* Build the menu against the same immutable selection later handed to the
       callback.  Status predicates must not observe the filmstrip's temporary
       selection or a hover update that happens while the menu is open. */
    dt_act_on_context_t *context = dt_act_on_push_context(images, image);
    GtkMenu *menu = _build_action_menu(&darktable.control->actions_thumb, 0, images, image);
    _append_more_actions(GTK_MENU_SHELL(menu), images, image);
    dt_act_on_pop_context(context);
    const gboolean populated = _menu_has_items(menu);
    g_list_free(images);
    if (!populated)
    {
        gtk_widget_destroy(GTK_WIDGET(menu));
        return FALSE;
    }

    dt_gui_menu_popup(menu, widget, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST);
    return TRUE;
}

static gboolean _action_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    (void)user_data;
    if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_SECONDARY)
    {
        dt_context_menu_image_provider_t *provider =
            _context_menu_image_quark ? g_object_get_qdata(G_OBJECT(widget),
                                                             _context_menu_image_quark) :
                                        NULL;
        if (provider)
            return dt_gui_context_menu_show_image(widget, provider->image_id(provider->user_data));

        dt_context_menu_widget_provider_t *widget_provider =
            _context_menu_widget_provider_quark ?
                g_object_get_qdata(G_OBJECT(widget), _context_menu_widget_provider_quark) :
                NULL;
        if (widget_provider && widget_provider->provider &&
            widget_provider->provider(widget, event, widget_provider->user_data))
            return TRUE;
        return dt_gui_context_menu_show_for_widget(widget);
    }

    return FALSE;
}

static gboolean _action_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void)user_data;
    if (event->keyval == GDK_KEY_Menu ||
        (event->keyval == GDK_KEY_F10 && (event->state & GDK_SHIFT_MASK)))
    {
        dt_context_menu_image_provider_t *provider =
            _context_menu_image_quark ? g_object_get_qdata(G_OBJECT(widget),
                                                             _context_menu_image_quark) :
                                        NULL;
        if (provider)
            return dt_gui_context_menu_show_image(widget, provider->image_id(provider->user_data));

        dt_context_menu_widget_provider_t *widget_provider =
            _context_menu_widget_provider_quark ?
                g_object_get_qdata(G_OBJECT(widget), _context_menu_widget_provider_quark) :
                NULL;
        if (widget_provider && widget_provider->provider &&
            widget_provider->provider(widget, NULL, widget_provider->user_data))
            return TRUE;
        return dt_gui_context_menu_show_for_widget(widget);
    }

    return FALSE;
}

void dt_gui_context_menu_attach(GtkWidget *widget)
{
    if (!widget)
        return;

    if (!_context_menu_quark)
        _context_menu_quark = g_quark_from_static_string("dt-context-menu-attached");
    if (g_object_get_qdata(G_OBJECT(widget), _context_menu_quark))
        return;

    g_object_set_qdata(G_OBJECT(widget), _context_menu_quark, GINT_TO_POINTER(1));
    gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(widget, "button-press-event", G_CALLBACK(_action_button_press), NULL);
    g_signal_connect(widget, "key-press-event", G_CALLBACK(_action_key_press), NULL);
}

void dt_gui_context_menu_attach_image(GtkWidget *widget,
                                      dt_gui_context_menu_image_id_callback_t image_id,
                                      gpointer user_data)
{
    if (!widget || !image_id)
        return;

    if (!_context_menu_image_quark)
        _context_menu_image_quark = g_quark_from_static_string("dt-context-menu-image-provider");

    dt_context_menu_image_provider_t *provider = g_malloc(sizeof(*provider));
    provider->image_id = image_id;
    provider->user_data = user_data;
    g_object_set_qdata_full(G_OBJECT(widget), _context_menu_image_quark, provider, g_free);
    dt_gui_context_menu_attach(widget);
}

void dt_gui_context_menu_attach_provider(GtkWidget *widget,
                                         dt_gui_context_menu_widget_provider_t provider,
                                         gpointer user_data, GDestroyNotify destroy_user_data)
{
    if (!widget || !provider)
        return;

    if (!_context_menu_widget_provider_quark)
        _context_menu_widget_provider_quark =
            g_quark_from_static_string("dt-context-menu-widget-provider");

    dt_context_menu_widget_provider_t *context_provider = g_malloc(sizeof(*context_provider));
    context_provider->provider = provider;
    context_provider->user_data = user_data;
    context_provider->destroy_user_data = destroy_user_data;
    g_object_set_qdata_full(G_OBJECT(widget), _context_menu_widget_provider_quark, context_provider,
                            _free_widget_provider);
    dt_gui_context_menu_attach(widget);
}
