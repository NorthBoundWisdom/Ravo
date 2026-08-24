/*
    This file is part of darktable.

    The former Objective-C++ implementation provided Cocoa-specific UI
    integration.  DarkTableNext currently uses GTK directly, so this C-only
    layer keeps the common application paths available without requiring an
    Objective-C++ compiler or Cocoa runtime.
*/

#include "osx.h"

#include "config.h"

#include <gio/gio.h>
#include <string.h>

#include "whereami.h"

float dt_osx_get_ppd()
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display)
        return 1.0f;

    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display) > 0)
        monitor = gdk_display_get_monitor(display, 0);

    if (!monitor)
        return 1.0f;

    const int scale = gdk_monitor_get_scale_factor(monitor);
    return scale > 0 ? (float)scale : 1.0f;
}

void dt_osx_disallow_fullscreen(GtkWidget *widget)
{
    (void)widget;
}

gboolean dt_osx_file_trash(const char *filename, GError **error)
{
    GFile *file = g_file_new_for_path(filename);
    const gboolean result = g_file_trash(file, NULL, error);
    g_object_unref(file);
    return result;
}

char *dt_osx_get_bundle_res_path()
{
    const int executable_length = wai_getExecutablePath(NULL, 0, NULL);
    if (executable_length <= 0)
        return NULL;

    char *executable_path = g_malloc(executable_length + 1);
    if (wai_getExecutablePath(executable_path, executable_length, NULL) != executable_length)
    {
        g_free(executable_path);
        return NULL;
    }
    executable_path[executable_length] = '\0';

    const char *const contents = strstr(executable_path, ".app/Contents/MacOS/");
    if (!contents)
    {
        g_free(executable_path);
        return NULL;
    }

    char *bundle_path = g_strndup(executable_path, contents - executable_path + 4);
    char *result = g_build_filename(bundle_path, "Contents", "Resources", NULL);
    g_free(bundle_path);
    g_free(executable_path);

    if (!g_file_test(result, G_FILE_TEST_IS_DIR))
        g_clear_pointer(&result, g_free);

    return result;
}

static char *_dt_osx_create_gdk_pixbuf_loader_cache(const char *resources_dir,
                                                     const char *loaders_dir)
{
    char *template_path =
        g_build_filename(resources_dir, "gtk-runtime", "gdk-pixbuf", "loaders.cache", NULL);
    if (!g_file_test(template_path, G_FILE_TEST_IS_REGULAR))
    {
        g_free(template_path);
        return NULL;
    }

    char *template_contents = NULL;
    gsize template_length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(template_path, &template_contents, &template_length, &error))
    {
        g_clear_error(&error);
        g_free(template_path);
        return NULL;
    }
    g_free(template_path);

#if DT_BUILD_DEVMODE
    char *cache_dir =
        g_build_filename(g_get_user_cache_dir(), "darktable-dev", DT_BUILD_CHECKOUT_ID, NULL);
#else
    char *cache_dir = g_build_filename(g_get_user_cache_dir(), "darktable", NULL);
#endif
    if (g_mkdir_with_parents(cache_dir, 0700) != 0)
    {
        g_free(cache_dir);
        g_free(template_contents);
        return NULL;
    }
    char *cache_path = g_build_filename(cache_dir, "gdk-pixbuf-loaders.cache", NULL);
    g_free(cache_dir);

    gchar **lines = g_strsplit(template_contents, "\n", -1);
    GString *relocated_cache = g_string_sized_new(template_length);
    for (gchar **line = lines; *line; line++)
    {
        const char *last_slash =
            (**line == '\"' && (*line)[1] == '/') ? g_strrstr(*line, "/") : NULL;
        if (last_slash)
        {
            // A loader record starts with its absolute module path.  Keep its
            // filename and metadata intact, but resolve it from this bundle.
            g_string_append_c(relocated_cache, '\"');
            g_string_append(relocated_cache, loaders_dir);
            g_string_append(relocated_cache, last_slash);
        }
        else
        {
            g_string_append(relocated_cache, *line);
        }
        if (*(line + 1))
            g_string_append_c(relocated_cache, '\n');
    }
    g_strfreev(lines);
    g_free(template_contents);

    const gboolean wrote_cache =
        g_file_set_contents(cache_path, relocated_cache->str, relocated_cache->len, &error);
    g_string_free(relocated_cache, TRUE);
    if (!wrote_cache)
    {
        g_clear_error(&error);
        g_free(cache_path);
        return NULL;
    }
    return cache_path;
}

void dt_osx_prepare_environment()
{
    char *resources_dir = dt_osx_get_bundle_res_path();
    if (!resources_dir)
        return;

    char *loaders_dir =
        g_build_filename(resources_dir, "gtk-runtime", "gdk-pixbuf", "loaders", NULL);
    if (!g_file_test(loaders_dir, G_FILE_TEST_IS_DIR))
    {
        g_free(loaders_dir);
        g_free(resources_dir);
        return;
    }

    if (!g_getenv("GDK_PIXBUF_MODULEDIR"))
        g_setenv("GDK_PIXBUF_MODULEDIR", loaders_dir, TRUE);

    if (!g_getenv("GDK_PIXBUF_MODULE_FILE"))
    {
        char *cache_path = _dt_osx_create_gdk_pixbuf_loader_cache(resources_dir, loaders_dir);
        if (cache_path)
        {
            g_setenv("GDK_PIXBUF_MODULE_FILE", cache_path, TRUE);
            g_free(cache_path);
        }
    }

    g_free(loaders_dir);
    g_free(resources_dir);
}

void dt_osx_focus_window()
{
}

gboolean dt_osx_open_url(const char *url)
{
    GError *error = NULL;
    const gboolean result = g_app_info_launch_default_for_uri(url, NULL, &error);
    g_clear_error(&error);
    return result;
}
