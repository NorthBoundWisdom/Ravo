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

#include "gui/system_commands.h"

#include "common/darktable.h"
#include "control/control.h"
#include "control/signal.h"
#include "gui/accelerators.h"
#include "views/view.h"

#include <string.h>

typedef enum dt_system_command_menu_t
{
    DT_SYSTEM_COMMAND_MENU_APP,
    DT_SYSTEM_COMMAND_MENU_FILE,
    DT_SYSTEM_COMMAND_MENU_EDIT,
    DT_SYSTEM_COMMAND_MENU_IMAGE,
    DT_SYSTEM_COMMAND_MENU_SELECTION,
    DT_SYSTEM_COMMAND_MENU_VIEW,
    DT_SYSTEM_COMMAND_MENU_LIGHTTABLE,
    DT_SYSTEM_COMMAND_MENU_DARKROOM,
    DT_SYSTEM_COMMAND_MENU_HELP,
    DT_SYSTEM_COMMAND_MENU_ACTIONS,
} dt_system_command_menu_t;

typedef struct dt_system_command_t
{
    gchar *action_id;
    gchar *native_name;
    dt_action_element_t element;
    dt_action_effect_t effect;
    int instance;
    dt_system_command_menu_t menu;
    gboolean stateful;
} dt_system_command_t;

static GtkApplication *_application = NULL;
static GtkWindow *_main_window = NULL;
static GPtrArray *_commands = NULL;
static GHashTable *_native_accelerators = NULL;
static guint _shortcuts_refresh_source = 0;

static void _refresh_states(void);
static void _sync_accelerators(void);

static void _free_command(gpointer data)
{
    dt_system_command_t *command = data;
    if (!command)
        return;

    g_free(command->action_id);
    g_free(command->native_name);
    g_free(command);
}

static gboolean _matches(const gchar *action_id, const gchar *prefix)
{
    return g_str_has_prefix(action_id, prefix) &&
           (action_id[strlen(prefix)] == '\0' || action_id[strlen(prefix)] == '/');
}

static dt_system_command_menu_t _menu_for_action(const gchar *action_id)
{
    if (!g_strcmp0(action_id, "global/about") || !g_strcmp0(action_id, "global/preferences") ||
        !g_strcmp0(action_id, "global/shortcuts") || !g_strcmp0(action_id, "global/quit") ||
        !g_strcmp0(action_id, "global/hide application") ||
        !g_strcmp0(action_id, "global/minimize window"))
        return DT_SYSTEM_COMMAND_MENU_APP;

    if (!g_strcmp0(action_id, "global/close window"))
        return DT_SYSTEM_COMMAND_MENU_FILE;

    if (!g_strcmp0(action_id, "global/documentation") ||
        !g_strcmp0(action_id, "global/homepage"))
        return DT_SYSTEM_COMMAND_MENU_HELP;

    if (_matches(action_id, "global/switch views") || _matches(action_id, "global/panels") ||
        !g_strcmp0(action_id, "global/fullscreen") ||
        !g_strcmp0(action_id, "global/toggle focus peaking") ||
        !g_strcmp0(action_id, "global/log history"))
        return DT_SYSTEM_COMMAND_MENU_VIEW;

    if (_matches(action_id, "views/lighttable"))
    {
        if (g_str_has_suffix(action_id, "/undo") || g_str_has_suffix(action_id, "/redo"))
            return DT_SYSTEM_COMMAND_MENU_EDIT;
        return DT_SYSTEM_COMMAND_MENU_LIGHTTABLE;
    }

    if (_matches(action_id, "views/darkroom"))
    {
        if (g_str_has_suffix(action_id, "/undo") || g_str_has_suffix(action_id, "/redo"))
            return DT_SYSTEM_COMMAND_MENU_EDIT;
        return DT_SYSTEM_COMMAND_MENU_DARKROOM;
    }

    if (_matches(action_id, "views/thumbtable"))
    {
        if (!g_strcmp0(action_id, "views/thumbtable/select all") ||
            !g_strcmp0(action_id, "views/thumbtable/select none") ||
            !g_strcmp0(action_id, "views/thumbtable/invert selection") ||
            !g_strcmp0(action_id, "views/thumbtable/select film roll") ||
            !g_strcmp0(action_id, "views/thumbtable/select untouched"))
            return DT_SYSTEM_COMMAND_MENU_SELECTION;
        return DT_SYSTEM_COMMAND_MENU_IMAGE;
    }

    if (_matches(action_id, "lib/import") || _matches(action_id, "lib/export"))
        return DT_SYSTEM_COMMAND_MENU_FILE;

    return DT_SYSTEM_COMMAND_MENU_ACTIONS;
}

static gboolean _is_stateful(const dt_action_t *action, const gchar *action_id,
                             const dt_action_element_t element)
{
    const dt_action_element_def_t *elements = dt_action_get_elements(action);
    return (elements && element >= 0 && elements[element].effects == dt_action_effect_toggle) ||
           !g_strcmp0(action_id, "global/toggle focus peaking");
}

static gchar *_native_action_name(const gchar *action_id, const dt_action_element_t element,
                                  const dt_action_effect_t effect, const int instance)
{
    gchar *identity = g_strdup_printf("%s;%d;%d;%d", action_id, element, effect, instance);
    gchar *checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, identity, -1);
    g_free(identity);

    gchar *normalised = g_ascii_strdown(action_id, -1);
    for (gchar *c = normalised; *c; c++)
        if (!g_ascii_isalnum(*c))
            *c = '-';

    gchar *name = g_strdup_printf("dt-%s-%.12s", normalised, checksum);
    g_free(normalised);
    g_free(checksum);
    return name;
}

static dt_system_command_t *_find_command(const gchar *action_id, const dt_action_element_t element,
                                          const dt_action_effect_t effect, const int instance)
{
    if (!_commands)
        return NULL;

    for (guint i = 0; i < _commands->len; i++)
    {
        dt_system_command_t *command = _commands->pdata[i];
        if (!g_strcmp0(command->action_id, action_id) && command->element == element &&
            command->effect == effect && command->instance == instance)
            return command;
    }

    return NULL;
}

static gboolean _eligible_action(const dt_action_t *action, const gboolean explicit)
{
    if (!action || action->context_menu_provider_only)
        return FALSE;

    return action->type == DT_ACTION_TYPE_COMMAND || explicit;
}

static void _command_activate(GSimpleAction *native_action, GVariant *parameter, gpointer user_data)
{
    const dt_system_command_t *command = user_data;
    dt_action_t *action = dt_action_find_by_id(command->action_id);
    if (!action)
    {
        _refresh_states();
        return;
    }

    dt_action_status_t status;
    dt_action_get_status(action, command->instance, command->element, command->effect, &status);
    if (status.applicable && status.enabled)
        dt_action_invoke(action, command->instance, command->element, command->effect, 1.0f);

    _refresh_states();
    (void)native_action;
    (void)parameter;
}

static void _add_command(dt_action_t *action, const gboolean explicit)
{
    if (!_eligible_action(action, explicit))
        return;

    gchar *action_id = dt_action_get_full_id(action);
    if (!action_id || _find_command(action_id, DT_ACTION_ELEMENT_DEFAULT,
                                    DT_ACTION_EFFECT_DEFAULT_KEY, 0))
    {
        g_free(action_id);
        return;
    }

    dt_system_command_t *command = g_malloc0(sizeof(*command));
    command->action_id = action_id;
    command->element = DT_ACTION_ELEMENT_DEFAULT;
    command->effect = DT_ACTION_EFFECT_DEFAULT_KEY;
    command->menu = _menu_for_action(command->action_id);
    command->stateful = _is_stateful(action, command->action_id, command->element);
    command->native_name = _native_action_name(command->action_id, command->element,
                                                command->effect, command->instance);

    GSimpleAction *native_action =
        command->stateful ?
            g_simple_action_new_stateful(command->native_name, NULL, g_variant_new_boolean(FALSE)) :
            g_simple_action_new(command->native_name, NULL);
    g_signal_connect(native_action, "activate", G_CALLBACK(_command_activate), command);
    g_action_map_add_action(G_ACTION_MAP(_application), G_ACTION(native_action));
    g_object_unref(native_action);

    g_ptr_array_add(_commands, command);
}

static void _collect_tree(const dt_action_t *root)
{
    for (const dt_action_t *action = dt_action_get_children(root); action; action = action->next)
    {
        _add_command((dt_action_t *)action, FALSE);
        _collect_tree(action);
    }
}

static void _collect_commands(void)
{
    _collect_tree(&darktable.control->actions_global);
    _collect_tree(&darktable.control->actions_views);
    _collect_tree(&darktable.control->actions_thumb);

    static const gchar *explicit_actions[] = {
        "global/preferences",         "global/shortcuts", "global/toggle focus peaking",
        "lib/import/add to library...", "lib/import/copy & import...",
        "lib/export/start export",    "lib/export/start batch export", NULL};
    for (const gchar **action_id = explicit_actions; *action_id; action_id++)
        _add_command(dt_action_find_by_id(*action_id), TRUE);
}

static gchar *_command_label(const dt_system_command_t *command)
{
    dt_action_t *action = dt_action_find_by_id(command->action_id);
    return action ? g_strdup(action->label) : g_strdup(command->action_id);
}

static void _append_command(GMenu *menu, const dt_system_command_t *command)
{
    gchar *label = _command_label(command);
    gchar *detailed_name = g_strdup_printf("app.%s", command->native_name);
    GMenuItem *item = g_menu_item_new(label, detailed_name);
    g_menu_append_item(menu, item);
    g_object_unref(item);
    g_free(detailed_name);
    g_free(label);
}

static gboolean _append_tree(GMenu *menu, const dt_action_t *root,
                             const dt_system_command_menu_t category)
{
    gboolean appended = FALSE;
    for (const dt_action_t *action = dt_action_get_children(root); action; action = action->next)
    {
        gchar *action_id = dt_action_get_full_id(action);
        dt_system_command_t *command =
            _find_command(action_id, DT_ACTION_ELEMENT_DEFAULT, DT_ACTION_EFFECT_DEFAULT_KEY, 0);
        if (command && command->menu == category)
        {
            _append_command(menu, command);
            appended = TRUE;
        }

        GMenu *submenu = g_menu_new();
        if (_append_tree(submenu, action, category))
        {
            g_menu_append_submenu(menu, action->label, G_MENU_MODEL(submenu));
            appended = TRUE;
        }
        g_object_unref(submenu);
        g_free(action_id);
    }

    return appended;
}

static gboolean _append_menu_for_root(GMenu *menubar, const gchar *label, const dt_action_t *root,
                                      const dt_system_command_menu_t category)
{
    GMenu *menu = g_menu_new();
    const gboolean appended = _append_tree(menu, root, category);
    if (appended)
        g_menu_append_submenu(menubar, label, G_MENU_MODEL(menu));
    g_object_unref(menu);
    return appended;
}

static void _append_flat_menu(GMenu *menubar, const gchar *label,
                              const dt_system_command_menu_t category)
{
    GMenu *menu = g_menu_new();
    for (guint i = 0; i < _commands->len; i++)
    {
        const dt_system_command_t *command = _commands->pdata[i];
        if (command->menu == category)
            _append_command(menu, command);
    }
    if (g_menu_model_get_n_items(G_MENU_MODEL(menu)))
        g_menu_append_submenu(menubar, label, G_MENU_MODEL(menu));
    g_object_unref(menu);
}

static const dt_action_t *_find_child(const dt_action_t *root, const gchar *id)
{
    for (const dt_action_t *child = dt_action_get_children(root); child; child = child->next)
        if (!g_strcmp0(child->id, id))
            return child;

    return NULL;
}

static void _append_view_menu(GMenu *menubar, const gchar *label, const gchar *view_id,
                              const dt_system_command_menu_t category)
{
    const dt_action_t *view = _find_child(&darktable.control->actions_views, view_id);
    if (view)
        _append_menu_for_root(menubar, label, view, category);
}

static void _rebuild_menus(void)
{
    GMenu *app_menu = g_menu_new();
    _append_tree(app_menu, &darktable.control->actions_global, DT_SYSTEM_COMMAND_MENU_APP);
    gtk_application_set_app_menu(_application, G_MENU_MODEL(app_menu));

    GMenu *menubar = g_menu_new();
    _append_flat_menu(menubar, _("File"), DT_SYSTEM_COMMAND_MENU_FILE);
    _append_menu_for_root(menubar, _("Edit"), &darktable.control->actions_views,
                          DT_SYSTEM_COMMAND_MENU_EDIT);
    _append_menu_for_root(menubar, _("Image"), &darktable.control->actions_thumb,
                          DT_SYSTEM_COMMAND_MENU_IMAGE);
    _append_menu_for_root(menubar, _("Selection"), &darktable.control->actions_thumb,
                          DT_SYSTEM_COMMAND_MENU_SELECTION);
    _append_menu_for_root(menubar, _("View"), &darktable.control->actions_global,
                          DT_SYSTEM_COMMAND_MENU_VIEW);
    _append_view_menu(menubar, _("Lighttable"), "lighttable",
                      DT_SYSTEM_COMMAND_MENU_LIGHTTABLE);
    _append_view_menu(menubar, _("Darkroom"), "darkroom",
                      DT_SYSTEM_COMMAND_MENU_DARKROOM);
    _append_menu_for_root(menubar, _("Actions"), &darktable.control->actions_global,
                          DT_SYSTEM_COMMAND_MENU_ACTIONS);
    _append_menu_for_root(menubar, _("Help"), &darktable.control->actions_global,
                          DT_SYSTEM_COMMAND_MENU_HELP);
    gtk_application_set_menubar(_application, G_MENU_MODEL(menubar));

    g_object_unref(menubar);
    g_object_unref(app_menu);
}

static guint64 _accelerator_key(const guint keyval, const GdkModifierType modifiers)
{
    return ((guint64)keyval << 32) | (guint32)modifiers;
}

static void _sync_accelerators(void)
{
    if (!_application || !_commands)
        return;

    g_hash_table_remove_all(_native_accelerators);
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint i = 0; i < _commands->len; i++)
    {
        const dt_system_command_t *command = _commands->pdata[i];
        dt_action_t *action = dt_action_find_by_id(command->action_id);
        gchar **available = dt_action_get_gtk_accels(action, command->instance, command->element,
                                                      command->effect);
        GPtrArray *selected = g_ptr_array_new_with_free_func(g_free);

        for (gchar **accelerator = available; accelerator && *accelerator; accelerator++)
        {
            guint keyval = 0;
            GdkModifierType modifiers = 0;
#ifdef __APPLE__
            if (g_str_has_prefix(*accelerator, "<Primary>"))
            {
                gtk_accelerator_parse(*accelerator + strlen("<Primary>"), &keyval, &modifiers);
                GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
                modifiers |= gdk_keymap_get_modifier_mask(
                    keymap, GDK_MODIFIER_INTENT_PRIMARY_ACCELERATOR);
            }
            else
                gtk_accelerator_parse(*accelerator, &keyval, &modifiers);
#else
            gtk_accelerator_parse(*accelerator, &keyval, &modifiers);
#endif
            const guint64 key = _accelerator_key(gdk_keyval_to_lower(keyval), modifiers);
            if (!keyval || g_hash_table_contains(names, *accelerator) ||
                g_hash_table_contains(_native_accelerators, (gpointer)(guintptr)key))
                continue;

            g_hash_table_add(names, g_strdup(*accelerator));
            g_hash_table_insert(_native_accelerators, (gpointer)(guintptr)key, (gpointer)command);
            g_ptr_array_add(selected, g_strdup(*accelerator));
        }
        g_ptr_array_add(selected, NULL);

        gchar *detailed_name = g_strdup_printf("app.%s", command->native_name);
        gtk_application_set_accels_for_action(_application, detailed_name,
                                              (const gchar *const *)selected->pdata);
        g_free(detailed_name);
        g_ptr_array_free(selected, TRUE);
        g_strfreev(available);
    }

    g_hash_table_destroy(names);
}

static void _refresh_states(void)
{
    if (!_application || !_commands)
        return;

    for (guint i = 0; i < _commands->len; i++)
    {
        const dt_system_command_t *command = _commands->pdata[i];
        dt_action_t *action = dt_action_find_by_id(command->action_id);

        GAction *native_action =
            g_action_map_lookup_action(G_ACTION_MAP(_application), command->native_name);
        if (!native_action)
            continue;

        if (!action)
        {
            g_simple_action_set_enabled(G_SIMPLE_ACTION(native_action), FALSE);
            continue;
        }

        dt_action_status_t status;
        dt_action_get_status(action, command->instance, command->element, command->effect,
                             &status);
        g_simple_action_set_enabled(G_SIMPLE_ACTION(native_action), status.applicable && status.enabled);
        if (command->stateful)
            g_simple_action_set_state(G_SIMPLE_ACTION(native_action),
                                      g_variant_new_boolean(status.checked));
    }

    _sync_accelerators();
}

static gboolean _refresh_shortcuts_idle(gpointer user_data)
{
    _shortcuts_refresh_source = 0;
    _sync_accelerators();
    (void)user_data;
    return G_SOURCE_REMOVE;
}

static void _refresh_signal(gpointer instance, gpointer user_data)
{
    _refresh_states();
    (void)instance;
    (void)user_data;
}

static void _refresh_view_signal(gpointer instance, dt_view_t *old_view, dt_view_t *new_view,
                                 gpointer user_data)
{
    _refresh_states();
    (void)instance;
    (void)old_view;
    (void)new_view;
    (void)user_data;
}

static void _rebuild_signal(gpointer instance, gpointer user_data)
{
    if (!_application)
        return;

    for (guint i = 0; i < _commands->len; i++)
    {
        const dt_system_command_t *command = _commands->pdata[i];
        g_action_map_remove_action(G_ACTION_MAP(_application), command->native_name);
    }
    g_ptr_array_set_size(_commands, 0);
    _collect_commands();
    _rebuild_menus();
    _refresh_states();
    (void)instance;
    (void)user_data;
}

void dt_gui_system_commands_init(GtkWindow *main_window)
{
    if (_application || !GTK_IS_WINDOW(main_window))
        return;

    _application = gtk_application_new("org.darktable.darktable", G_APPLICATION_NON_UNIQUE);
    GError *error = NULL;
    if (!g_application_register(G_APPLICATION(_application), NULL, &error))
    {
        dt_print(DT_DEBUG_ALWAYS, "[system commands] unable to register GtkApplication: %s",
                 error ? error->message : "unknown error");
        g_clear_error(&error);
        g_clear_object(&_application);
        return;
    }

    _main_window = main_window;
    gtk_application_add_window(_application, _main_window);
    _commands = g_ptr_array_new_with_free_func(_free_command);
    _native_accelerators = g_hash_table_new(g_direct_hash, g_direct_equal);

    _collect_commands();
    _rebuild_menus();
    _refresh_states();

    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED, _refresh_view_signal, _commands);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_SELECTION_CHANGED, _refresh_signal, _commands);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_ACTIVE_IMAGES_CHANGE, _refresh_signal, _commands);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _refresh_signal, _commands);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _refresh_signal, _commands);
    DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_STYLE_CHANGED, _rebuild_signal, _commands);
}

void dt_gui_system_commands_cleanup(void)
{
    if (_shortcuts_refresh_source)
    {
        g_source_remove(_shortcuts_refresh_source);
        _shortcuts_refresh_source = 0;
    }

    if (_commands && darktable.signals)
        dt_control_signal_disconnect_all(darktable.signals, _commands);

    if (_application && _commands)
    {
        for (guint i = 0; i < _commands->len; i++)
        {
            const dt_system_command_t *command = _commands->pdata[i];
            g_action_map_remove_action(G_ACTION_MAP(_application), command->native_name);
        }
    }
    g_clear_pointer(&_commands, g_ptr_array_unref);
    g_clear_pointer(&_native_accelerators, g_hash_table_destroy);

    if (_application && _main_window)
        gtk_application_remove_window(_application, _main_window);
    _main_window = NULL;
    g_clear_object(&_application);
}

void dt_gui_system_commands_shortcuts_changed(void)
{
    if (!_application || _shortcuts_refresh_source)
        return;

    _shortcuts_refresh_source = g_idle_add(_refresh_shortcuts_idle, NULL);
}

gboolean dt_gui_system_commands_activate_key_event(const GdkEventKey *event)
{
    if (!_native_accelerators || !event || !event->hardware_keycode)
        return FALSE;

    guint keyval = 0;
    GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
    gdk_keymap_translate_keyboard_state(keymap, event->hardware_keycode, 0, 0, &keyval, NULL,
                                        NULL, NULL);
    const GdkModifierType modifiers = dt_shortcut_normalize_modifiers(event->state);
    dt_system_command_t *command =
        g_hash_table_lookup(_native_accelerators,
                            (gpointer)(guintptr)_accelerator_key(gdk_keyval_to_lower(keyval),
                                                                 modifiers));
    if (!command)
        return FALSE;

    _command_activate(NULL, NULL, command);
    return TRUE;
}
