#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean dt_win_file_trash(GFile *file, GCancellable *cancellable, GError **error);

G_END_DECLS
