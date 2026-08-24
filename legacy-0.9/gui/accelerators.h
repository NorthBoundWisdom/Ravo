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

#pragma once

#include <stdint.h>
#include <gtk/gtk.h>

#include "common/darktable_api.h"
#include "develop/imageop.h"
#include "libs/lib.h"
#include "views/view.h"

G_BEGIN_DECLS

GtkWidget *dt_shortcuts_prefs(GtkWidget *widget);
GHashTable *dt_shortcut_category_lists(dt_view_type_flags_t v);

void dt_shortcuts_save(const gchar *ext, const gboolean backup);

// load the contents of a shortcutsrc file
// if 'ext' points at a string containing a directory separator, it is taken
//   to be the path of the file to load
// otherwise, a shortcuts file will be loaded from the user configuration directory,
//  and the contents of 'ext' (if non-NULL) wlil be appended to the default name.
void dt_shortcuts_load(const gchar *ext, const gboolean clear);

void dt_shortcuts_reinitialise(dt_action_t *action);

void dt_shortcuts_select_view(dt_view_type_flags_t view);

gboolean dt_shortcut_dispatcher(GtkWidget *w, GdkEvent *event, gpointer user_data);
gboolean dt_shortcut_tooltip_callback(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode,
                                      GtkTooltip *tooltip, GtkWidget *vbox);
/** Apply the current platform's shortcut modifier policy to an input event. */
GdkModifierType dt_shortcut_normalize_modifiers(GdkModifierType modifiers);

float dt_action_process(const gchar *action, int instance, const gchar *element,
                        const gchar *effect, float size);

void dt_action_insert_sorted(dt_action_t *owner, dt_action_t *new_action);

dt_action_t *dt_action_locate(dt_action_t *owner, gchar **path, gboolean create);

static inline dt_action_t *dt_action_section(dt_action_t *owner, const gchar *section)
{
    gchar *path[] = { (gchar *)section, NULL };
    return dt_action_locate(owner, path, TRUE);
}
static inline dt_view_t *dt_action_view(dt_action_t *action)
{
    while (action && action->type != DT_ACTION_TYPE_VIEW)
        action = action->owner;
    return (dt_view_t *)action;
}
static inline dt_lib_module_t *dt_action_lib(dt_action_t *action)
{
    while (action && action->type != DT_ACTION_TYPE_LIB)
        action = action->owner;
    return (dt_lib_module_t *)action;
}

void dt_action_define_preset(dt_action_t *action, const gchar *name);
// rename or remove (new_name == NULL) actions or presets
void dt_action_rename_preset(dt_action_t *action, const gchar *old_name, const gchar *new_name);
void dt_action_rename(dt_action_t *action, const gchar *new_name);

typedef uint8_t dt_input_device_t;

// FIXME this could eventually be refactored into dt_input_module_t
// with its own _api.h and loader
typedef struct dt_input_driver_definition_t
{
    gchar *name;
    gchar *(*key_to_string)(const guint key, const gboolean display);
    gboolean (*string_to_key)(const gchar *string, guint *key);
    gchar *(*move_to_string)(const guint move, const gboolean display);
    gboolean (*string_to_move)(const gchar *string, guint *move);
    gboolean (*key_to_move)(dt_lib_module_t *self, const dt_input_device_t id, const guint key,
                            guint *move);
    dt_lib_module_t *module;
} dt_input_driver_definition_t;

dt_input_device_t dt_register_input_driver(dt_lib_module_t *module,
                                           const dt_input_driver_definition_t *callbacks);
void dt_shortcut_key_press(dt_input_device_t id, const guint time, const guint key);
void dt_shortcut_key_release(dt_input_device_t id, const guint time, const guint key);
gboolean dt_shortcut_key_active(dt_input_device_t id, const guint key);
float dt_shortcut_move(dt_input_device_t id, const guint time, const guint move,
                       const float move_size);

typedef enum dt_shortcut_flag_t
{
    DT_SHORTCUT_LONG = 1 << 0,
    DT_SHORTCUT_DOUBLE = 1 << 1,
    DT_SHORTCUT_TRIPLE = 1 << 2,
    DT_SHORTCUT_LEFT = 1 << 0,
    DT_SHORTCUT_MIDDLE = 1 << 1,
    DT_SHORTCUT_RIGHT = 1 << 2,
    DT_SHORTCUT_DOWN = 1 << 0,
    DT_SHORTCUT_UP = 1 << 1,
} dt_shortcut_flag_t;

typedef enum dt_shortcut_move_t
{
    DT_SHORTCUT_MOVE_NONE,
    DT_SHORTCUT_MOVE_SCROLL,
    DT_SHORTCUT_MOVE_PAN,
    DT_SHORTCUT_MOVE_HORIZONTAL,
    DT_SHORTCUT_MOVE_VERTICAL,
    DT_SHORTCUT_MOVE_DIAGONAL,
    DT_SHORTCUT_MOVE_SKEW,
    DT_SHORTCUT_MOVE_LEFTRIGHT, // FIXME cursor key pairs will be treated as Moves
    DT_SHORTCUT_MOVE_UPDOWN,    // once per-view key_pressed routines that extensively use them
    DT_SHORTCUT_MOVE_PGUPDOWN,  // are ported to the action framework
} dt_shortcut_move_t;

#define DT_ACTION_EFFECT_TAG_VALUE ((const gchar **)(uintptr_t)0x101)
#define DT_ACTION_EFFECT_TAG_SELECTION ((const gchar **)(uintptr_t)0x102)
#define DT_ACTION_EFFECT_TAG_TOGGLE ((const gchar **)(uintptr_t)0x103)
#define DT_ACTION_EFFECT_TAG_HOLD ((const gchar **)(uintptr_t)0x104)
#define DT_ACTION_EFFECT_TAG_ACTIVATE ((const gchar **)(uintptr_t)0x105)
#define DT_ACTION_EFFECT_TAG_PRESETS ((const gchar **)(uintptr_t)0x106)
#define DT_ACTION_EFFECT_TAG_CYCLE ((const gchar **)(uintptr_t)0x107)

#if defined(_WIN32) && !defined(DT_LIB_DARKTABLE_BUILD)
#define dt_action_effect_value DT_ACTION_EFFECT_TAG_VALUE
#define dt_action_effect_selection DT_ACTION_EFFECT_TAG_SELECTION
#define dt_action_effect_toggle DT_ACTION_EFFECT_TAG_TOGGLE
#define dt_action_effect_hold DT_ACTION_EFFECT_TAG_HOLD
#define dt_action_effect_activate DT_ACTION_EFFECT_TAG_ACTIVATE
#define dt_action_effect_presets DT_ACTION_EFFECT_TAG_PRESETS
#define dt_action_effect_cycle DT_ACTION_EFFECT_TAG_CYCLE
#else
extern DT_CORE_API const gchar *dt_action_effect_value[];
extern DT_CORE_API const gchar *dt_action_effect_selection[];
extern DT_CORE_API const gchar *dt_action_effect_toggle[];
extern DT_CORE_API const gchar *dt_action_effect_hold[];
extern DT_CORE_API const gchar *dt_action_effect_activate[];
extern DT_CORE_API const gchar *dt_action_effect_presets[];
extern DT_CORE_API const gchar *dt_action_effect_cycle[];
#endif

typedef struct dt_action_element_def_t
{
    const gchar *name;
    const gchar **effects;
} dt_action_element_def_t;

#define DT_ACTION_ELEMENTS_NUM(effect)                                                             \
    (dt_action_element_def_t[])                                                                    \
    {                                                                                              \
        {N_("1st"), dt_action_effect_##effect}, {N_("2nd"), dt_action_effect_##effect},            \
            {N_("3rd"), dt_action_effect_##effect}, {N_("4th"), dt_action_effect_##effect},        \
            {N_("5th"), dt_action_effect_##effect}, {N_("6th"), dt_action_effect_##effect},        \
            {N_("7th"), dt_action_effect_##effect}, {N_("8th"), dt_action_effect_##effect},        \
            {N_("9th"), dt_action_effect_##effect}, {N_("10th"), dt_action_effect_##effect},       \
            {N_("11th"), dt_action_effect_##effect}, {N_("12th"), dt_action_effect_##effect},      \
            {N_("13th"), dt_action_effect_##effect}, {N_("14th"), dt_action_effect_##effect},      \
            {N_("15th"), dt_action_effect_##effect}, {N_("16th"), dt_action_effect_##effect},      \
            {N_("17th"), dt_action_effect_##effect}, {N_("18th"), dt_action_effect_##effect},      \
            {N_("19th"), dt_action_effect_##effect}, {N_("20th"), dt_action_effect_##effect},      \
        {                                                                                          \
        }                                                                                          \
    }

#define DT_ACTION_ELEMENTS_TAG_HOLD ((const dt_action_element_def_t *)(uintptr_t)0x201)

#if defined(_WIN32) && !defined(DT_LIB_DARKTABLE_BUILD)
#define dt_action_elements_hold DT_ACTION_ELEMENTS_TAG_HOLD
#else
extern DT_CORE_API const dt_action_element_def_t dt_action_elements_hold[];
#endif

typedef struct dt_shortcut_fallback_t
{
    guint mods;
    guint press : 3;
    guint button : 3;
    guint click : 3;
    guint direction : 2;
    dt_shortcut_move_t move;
    dt_action_element_t element;
    dt_action_effect_t effect;
    float speed;
} dt_shortcut_fallback_t;

#define DT_VALUE_PATTERN_PLUS_MINUS 2.f
#define DT_VALUE_PATTERN_PERCENTAGE 4.f
#define DT_VALUE_PATTERN_ACTIVE -1.f / 2
#define DT_VALUE_PATTERN_SUM -1.f / 4

typedef struct dt_action_def_t
{
    const gchar *name;
    float (*process)(gpointer target, dt_action_element_t, dt_action_effect_t, float size);
    const dt_action_element_def_t *elements;
    const dt_shortcut_fallback_t *fallbacks;
    const gboolean no_widget;
} dt_action_def_t;

#define DT_ACTION_DEF_TAG_TOGGLE ((const dt_action_def_t *)(uintptr_t)0x301)
#define DT_ACTION_DEF_TAG_BUTTON ((const dt_action_def_t *)(uintptr_t)0x302)
#define DT_ACTION_DEF_TAG_ENTRY ((const dt_action_def_t *)(uintptr_t)0x303)
#define DT_ACTION_DEF_TAG_VALUE ((const dt_action_def_t *)(uintptr_t)0x304)

#if defined(_WIN32) && !defined(DT_LIB_DARKTABLE_BUILD)
#define dt_action_def_toggle (*DT_ACTION_DEF_TAG_TOGGLE)
#define dt_action_def_button (*DT_ACTION_DEF_TAG_BUTTON)
#define dt_action_def_entry (*DT_ACTION_DEF_TAG_ENTRY)
#define dt_action_def_value (*DT_ACTION_DEF_TAG_VALUE)
#else
extern DT_CORE_API const dt_action_def_t dt_action_def_toggle;
extern DT_CORE_API const dt_action_def_t dt_action_def_button;
extern DT_CORE_API const dt_action_def_t dt_action_def_entry;
extern DT_CORE_API const dt_action_def_t dt_action_def_value;
#endif

/** Read-only availability and state information for an action invocation.
 *
 * Context-menu providers use this instead of probing an action by executing it.
 * `value` is the action-specific read-only value when the action exposes one,
 * otherwise DT_ACTION_NOT_VALID. */
typedef struct dt_action_status_t
{
    gboolean applicable;
    gboolean enabled;
    gboolean checked;
    gboolean inconsistent;
    float value;
    const gchar *reason;
} dt_action_status_t;

/** Return the action definition and element table associated with an action. */
const dt_action_def_t *dt_action_get_definition(const dt_action_t *action);
const dt_action_element_def_t *dt_action_get_elements(const dt_action_t *action);
const dt_action_t *dt_action_get_children(const dt_action_t *action);
int dt_action_get_element_count(const dt_action_t *action);
int dt_action_get_effect_count(const dt_action_t *action, dt_action_element_t element);
int dt_action_get_combo_count(const dt_action_t *action, dt_action_element_t element);

/** Return the views in which an action is applicable according to its owner. */
dt_view_type_flags_t dt_action_get_views(const dt_action_t *action);

/** Return newly allocated, display-ready labels. The caller owns the result. */
gchar *dt_action_get_full_id(const dt_action_t *action);
gchar *dt_action_get_full_label(const dt_action_t *action);
gchar *dt_action_get_effect_label(const dt_action_t *action, dt_action_element_t element,
                                  dt_action_effect_t effect);
gchar *dt_action_get_shortcut_label(const dt_action_t *action, int instance,
                                    dt_action_element_t element, dt_action_effect_t effect);

/** Resolve a stable, escaped Action ID previously returned by dt_action_get_full_id(). */
dt_action_t *dt_action_find_by_id(const gchar *action_id);

/** Return the active-view shortcuts that GTK can represent as native accelerators.
 * The result is a newly allocated, NULL-terminated list suitable for
 * gtk_application_set_accels_for_action(); free it with g_strfreev(). */
gchar **dt_action_get_gtk_accels(const dt_action_t *action, int instance,
                                 dt_action_element_t element, dt_action_effect_t effect);

/** Find the closest action assigned to widget or one of its ancestors.
 * `action_widget`, when non-NULL, receives the widget that owns the action. */
dt_action_t *dt_action_find_widget(GtkWidget *widget, GtkWidget **action_widget);

/** Resolve the relative IOP instance represented by widget for an action. */
gboolean dt_action_get_instance(const dt_action_t *action, GtkWidget *widget, int *instance);

/** Query an action without changing application state. */
void dt_action_get_status(dt_action_t *action, int instance, dt_action_element_t element,
                          dt_action_effect_t effect, dt_action_status_t *status);

/** Attach a domain-owned, side-effect-free status refinement to an Action.
 * The callback runs after generic view/target validation and may disable an
 * otherwise valid invocation with a contextual reason. `user_data` remains
 * owned by the Action owner and must outlive the Action. */
void dt_action_set_status_callback(dt_action_t *action, dt_action_status_callback_t *callback,
                                   gpointer user_data);

/** Keep an Action available to an explicit context-menu provider while
 * suppressing it from generic owner-menu traversal. */
void dt_action_set_context_menu_provider_only(dt_action_t *action, gboolean provider_only);

/** Invoke an already resolved action. This is the pointer-based equivalent of
 * dt_action_process() and performs the same current-view validation. */
float dt_action_invoke(dt_action_t *action, int instance, dt_action_element_t element,
                       dt_action_effect_t effect, float move_size);

dt_action_t *dt_action_define_iop(dt_iop_module_t *self, const gchar *section, const gchar *label,
                                  GtkWidget *widget, const dt_action_def_t *action_def);

dt_action_t *dt_action_define(dt_action_t *owner, const gchar *section, const gchar *label,
                              GtkWidget *widget, const dt_action_def_t *action_def);

void dt_action_define_fallback(dt_action_type_t type, const dt_action_def_t *action_def);

typedef enum dt_accel_iop_slider_scale_t
{
    DT_IOP_PRECISION_NORMAL = 0,
    DT_IOP_PRECISION_FINE = 1,
    DT_IOP_PRECISION_COARSE = 2
} dt_accel_iop_slider_scale_t;

typedef void dt_action_callback_t(dt_action_t *action);
dt_action_t *dt_action_register(dt_action_t *owner, const gchar *label,
                                dt_action_callback_t callback, guint accel_key,
                                GdkModifierType mods);
void dt_shortcut_register(dt_action_t *owner, guint element, guint effect, guint accel_key,
                          GdkModifierType mods);

// Accelerator connection functions
void dt_accel_connect_instance_iop(dt_iop_module_t *module);

// Cleanup function
void dt_action_cleanup_instance_iop(dt_iop_module_t *module);

// UX miscellaneous functions
void dt_action_widget_toast(dt_action_t *action, GtkWidget *widget, const gchar *msg, ...);

// check if widget intentionally hidden (to disable it)
gboolean dt_action_widget_invisible(GtkWidget *w);

// Get the speed multiplier for adjusting sliders and other widgets
float dt_accel_get_speed_multiplier(GtkWidget *widget, guint state);

// create a shortcutable button with ellipsized label and tooltip
GtkWidget *dt_action_button_new(dt_lib_module_t *self, const gchar *label, gpointer callback,
                                gpointer data, const gchar *tooltip, guint accel_key,
                                GdkModifierType mods);

// create a shortcutable entry field
GtkWidget *dt_action_entry_new(dt_action_t *ac, const gchar *label, gpointer callback,
                               gpointer data, const gchar *tooltip, const gchar *text);

// find the action a widget is linked to
dt_action_t *dt_action_widget(GtkWidget *widget);

G_END_DECLS
