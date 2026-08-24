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

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/** Initialise the GtkApplication command projection for the existing main window.
 *
 * This does not own darktable's legacy GTK event loop.  It registers the application,
 * its GActions and its GMenuModel so platform backends can expose the same commands
 * through their native menu integration. */
void dt_gui_system_commands_init(GtkWindow *main_window);

/** Remove application actions and menu integration before the Action tree is released. */
void dt_gui_system_commands_cleanup(void);

/** Coalesce a native accelerator refresh after the shortcut model changes. */
void dt_gui_system_commands_shortcuts_changed(void);

/** Activate the native GAction for a simple keyboard accelerator registered in the menu.
 * The legacy GTK main-loop dispatcher calls this only after focused controls declined the event. */
gboolean dt_gui_system_commands_activate_key_event(const GdkEventKey *event);

G_END_DECLS
