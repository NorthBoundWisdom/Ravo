/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

#include "common/action.h"
#include "common/darktable.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

/** Add the standard Action context-menu and keyboard handlers to a widget.
 * It is idempotent and intentionally only applies to explicitly registered
 * Action widgets. */
void dt_gui_context_menu_attach(GtkWidget *widget);

typedef dt_imgid_t (*dt_gui_context_menu_image_id_callback_t)(gpointer user_data);
typedef gboolean (*dt_gui_context_menu_widget_provider_t)(GtkWidget *widget,
                                                          const GdkEventButton *event,
                                                          gpointer user_data);

/** Mark a widget as a hit target for an image context menu.  This takes
 * precedence over the widget's own Action so thumbnail overlays expose the
 * complete image action set rather than just their individual control. */
void dt_gui_context_menu_attach_image(GtkWidget *widget,
                                      dt_gui_context_menu_image_id_callback_t image_id,
                                      gpointer user_data);

/** Override the generic Action projection for a widget that owns a richer
 * provider menu, such as a text editor retaining GTK's native clipboard
 * entries. The provider may return FALSE to fall back to the normal Action
 * menu. Image providers retain precedence over this callback. */
void dt_gui_context_menu_attach_provider(GtkWidget *widget,
                                         dt_gui_context_menu_widget_provider_t provider,
                                         gpointer user_data, GDestroyNotify destroy_user_data);

/** Show the menu for the closest Action associated with widget. */
gboolean dt_gui_context_menu_show_for_widget(GtkWidget *widget);

/** Show all discrete thumbnail actions against a stable image target. */
gboolean dt_gui_context_menu_show_image(GtkWidget *widget, dt_imgid_t image);

/** Append a contiguous subset of one Action's discrete elements to a provider
 * menu. This lets a graph provider retain its ordinary Action controls while
 * adding hit-tested object operations with their own payload. */
gboolean dt_gui_context_menu_append_action_elements(GtkMenuShell *shell, dt_action_t *action,
                                                     int instance, int first_element,
                                                     int element_count);

/** Create a menu item that invokes an existing Action against provider-owned
 * context. The payload is only visible synchronously from the Action callback
 * through dt_gui_context_menu_get_action_payload() and is destroyed with the
 * menu item. */
GtkWidget *dt_gui_context_menu_action_item_new(const gchar *label, dt_action_t *action,
                                               int instance, dt_action_element_t element,
                                               dt_action_effect_t effect, gpointer payload,
                                               GDestroyNotify destroy_payload);

/** Bind an existing provider-created menu item to an Action. This preserves
 * provider-specific GtkMenuItem subclasses (for example check items) while
 * retaining the same target snapshot, status validation and invocation path
 * as a projected Action item. */
void dt_gui_context_menu_bind_action_item(GtkMenuItem *item, dt_action_t *action, int instance,
                                          dt_action_element_t element, dt_action_effect_t effect,
                                          gpointer payload, GDestroyNotify destroy_payload);

/** Return the immutable provider payload for the Action currently being
 * invoked from a context menu, or NULL for all other invocation paths. */
gpointer dt_gui_context_menu_get_action_payload(const dt_action_t *action);

G_END_DECLS
