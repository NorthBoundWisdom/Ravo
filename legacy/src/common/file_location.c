/*
    This file is part of darktable,
    Copyright (C) 2012-2025 darktable developers.

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

/* getpwnam_r availability check */
#if defined __APPLE__ || defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE ||                  \
    defined _BSD_SOURCE || defined _SVID_SOURCE || defined _POSIX_SOURCE ||                        \
    defined __DragonFly__ || defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__

#include <pwd.h>
#include <sys/types.h>
#define HAVE_GETPWNAM_R 1
#endif

#ifdef __APPLE__
#include "osx/osx.h"
#endif

#include "common/grealpath.h"
#include "darktable.h"
#include "file_location.h"
#include "whereami.h"

static gchar *_default_user_directory(const gchar *base_directory)
{
#if DT_BUILD_DEVMODE
    return g_build_filename(base_directory, "darktable-dev", DT_BUILD_CHECKOUT_ID, NULL);
#else
    return g_build_filename(base_directory, "darktable", NULL);
#endif
}

uint8_t dt_loc_init(const char *datadir, const char *moduledir, const char *configdir,
                    const char *cachedir, const char *tmpdir)
{
    // Assemble pathes
    char *application_directory = NULL;
    int dirname_length;
    // calling wai_getExecutablePath twice as recommended in the docs:
    // the first call retrieves the length of the path
    int length = wai_getExecutablePath(NULL, 0, &dirname_length);
    if (length > 0)
    {
        application_directory = (char *)malloc(length + 1);
        // the second call retrieves the path including the executable
        wai_getExecutablePath(application_directory, length, &dirname_length);
        // strip of the executable name from the path to retrieve the path alone
        application_directory[dirname_length] = '\0';
    }
    dt_print(DT_DEBUG_DEV, "application_directory: %s", application_directory);

    // set up absolute pathes based on their relative value
    dt_loc_init_datadir(application_directory, datadir);
    dt_loc_init_plugindir(application_directory, moduledir);
    dt_loc_init_sharedir(application_directory);

    free(application_directory);

    if (!dt_loc_init_user_config_dir(configdir))
        return CONFIGDIR_CREATION_FAILED;
    if (!dt_loc_init_user_cache_dir(cachedir))
        return CACHEDIR_CREATION_FAILED;
    if (!dt_loc_init_tmp_dir(tmpdir))
        return TMPDIR_CREATION_FAILED;

    return 0;
}

gchar *dt_loc_get_home_dir(const gchar *user)
{
    if (user == NULL || g_strcmp0(user, g_get_user_name()) == 0)
    {
        const char *home_dir = g_getenv("HOME");
        return g_strdup((home_dir != NULL) ? home_dir : g_get_home_dir());
    }

#if defined HAVE_GETPWNAM_R
    /* if the given username is not the same as the current one, we try
   * to retrieve the pw dir from the password file entry */
    struct passwd pwd;
    struct passwd *result;
#ifdef _SC_GETPW_R_SIZE_MAX
    int bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize < 0)
    {
        bufsize = 4096;
    }
#else
    int bufsize = 4096;
#endif

    gchar *buffer = g_malloc0_n(bufsize, sizeof(gchar));
    if (buffer == NULL)
    {
        return NULL;
    }

    getpwnam_r(user, &pwd, buffer, bufsize, &result);
    if (result == NULL)
    {
        g_free(buffer);
        return NULL;
    }

    gchar *dir = g_strdup(pwd.pw_dir);
    g_free(buffer);

    return dir;
#else
    return NULL;
#endif
}

gchar *dt_loc_init_generic(const char *absolute_value, const char *application_directory,
                           const char *default_value)
{
    gchar *result = NULL;
    gchar *path = NULL;

    if (absolute_value)
    {
        // the only adjustment the absolute path needs is
        // transforming the possible tilde '~' to an absolute path
        path = dt_util_fix_path(absolute_value);
    }
    else
    {
        // the default_value could be absolute or relative.
        // we decide upon presence of the application_directory.
        if (application_directory)
        {
            // default_value is relative.
            // combine basename (application_directory) and relative path (default_value).
            gchar complete_path[PATH_MAX] = {0};
#if defined(__APPLE__)
            char *bundle_path = dt_osx_get_bundle_res_path();
            if (bundle_path)
            {
                // Bundle resources replace the normal ../lib and ../share layout.  The bundle path
                // is resolved from the actual executable, so this remains valid through the
                // bin/darktable compatibility wrapper.
                const char *resource_relative =
                    g_str_has_prefix(default_value, "..") ? default_value + 2 : default_value;
                g_snprintf(complete_path, sizeof(complete_path), "%s%s", bundle_path,
                           resource_relative);
                g_free(bundle_path);
            }
            else
            {
                /// outside a bundle. Apply standard linux path rules
                g_snprintf(complete_path, sizeof(complete_path), "%s/%s", application_directory,
                           default_value);
            }
#else
            g_snprintf(complete_path, sizeof(complete_path), "%s/%s", application_directory,
                       default_value);
#endif
            path = g_strdup(complete_path);
        }
        else
        {
            // default_value is absolute
            path = g_strdup(default_value);
        }
    }

    // create file if it does not exist
    if (g_file_test(path, G_FILE_TEST_EXISTS) == FALSE)
        g_mkdir_with_parents(path, 0700);

    // removes '.', '..', and extra '/' characters.
    result = g_realpath(path);

    g_free(path);
    return result;
}

gboolean dt_loc_init_user_config_dir(const char *configdir)
{
    char *default_config_dir = _default_user_directory(g_get_user_config_dir());
    darktable.configdir = dt_loc_init_generic(configdir, NULL, default_config_dir);
    g_free(default_config_dir);
    return dt_check_opendir("darktable.configdir", darktable.configdir);
}

gboolean dt_loc_init_tmp_dir(const char *tmpdir)
{
    darktable.tmpdir = dt_loc_init_generic(tmpdir, NULL, g_get_tmp_dir());
    return dt_check_opendir("darktable.tmpdir", darktable.tmpdir);
}

gboolean dt_loc_init_user_cache_dir(const char *cachedir)
{
    char *default_cache_dir = _default_user_directory(g_get_user_cache_dir());
    darktable.cachedir = dt_loc_init_generic(cachedir, NULL, default_cache_dir);
    g_free(default_cache_dir);
    return dt_check_opendir("darktable.cachedir", darktable.cachedir);
}

void dt_loc_init_plugindir(const char *application_directory, const char *plugindir)
{
    darktable.plugindir = dt_loc_init_generic(plugindir, application_directory, DARKTABLE_LIBDIR);
    dt_check_opendir("darktable.plugindir", darktable.plugindir);
}

gboolean dt_check_opendir(const char *context, const char *directory)
{
    if (!directory)
    {
        dt_print(DT_DEBUG_ALWAYS, "directory for %s has not been set", context);
        return FALSE;
    }

#if _WIN32
    wchar_t *wdirectory = g_utf8_to_utf16(directory, -1, NULL, NULL, NULL);
    DWORD attribs = GetFileAttributesW(wdirectory);
    g_free(wdirectory);
    if (attribs != INVALID_FILE_ATTRIBUTES && (attribs & FILE_ATTRIBUTE_DIRECTORY))
    {
        dt_print(DT_DEBUG_DEV, "%s: %s", context, directory);
    }
    else
    {
        dt_print(DT_DEBUG_ALWAYS, "%s: directory '%s' fails to open", context, directory);
        return FALSE;
    }
#else
    DIR *dir = opendir(directory);
    if (dir)
    {
        dt_print(DT_DEBUG_DEV, "%s: %s", context, directory);
        closedir(dir);
    }
    else
    {
        dt_print(DT_DEBUG_ALWAYS, "opendir '%s' fails with: '%s'", directory, strerror(errno));
        return FALSE;
    }
#endif
    return TRUE;
}

void dt_loc_init_datadir(const char *application_directory, const char *datadir)
{
    darktable.datadir = dt_loc_init_generic(datadir, application_directory, DARKTABLE_DATADIR);
    dt_check_opendir("darktable.datadir", darktable.datadir);
}

void dt_loc_init_sharedir(const char *application_directory)
{
    darktable.sharedir = dt_loc_init_generic(NULL, application_directory, DARKTABLE_SHAREDIR);
    dt_check_opendir("darktable.sharedir", darktable.sharedir);
}

void dt_loc_get_kerneldir(char *kerneldir, size_t bufsize)
{
    char datadir[PATH_MAX] = {0};
    dt_loc_get_datadir(datadir, sizeof(datadir));
    snprintf(kerneldir, bufsize, "%s" G_DIR_SEPARATOR_S "kernels", datadir);
}

void dt_loc_get_plugindir(char *plugindir, size_t bufsize)
{
    g_strlcpy(plugindir, darktable.plugindir, bufsize);
}

void dt_loc_get_user_config_dir(char *configdir, size_t bufsize)
{
    g_strlcpy(configdir, darktable.configdir, bufsize);
}
void dt_loc_get_user_cache_dir(char *cachedir, size_t bufsize)
{
    g_strlcpy(cachedir, darktable.cachedir, bufsize);
}
void dt_loc_get_tmp_dir(char *tmpdir, size_t bufsize)
{
    g_strlcpy(tmpdir, darktable.tmpdir, bufsize);
}
void dt_loc_get_datadir(char *datadir, size_t bufsize)
{
    g_strlcpy(datadir, darktable.datadir, bufsize);
}
void dt_loc_get_sharedir(char *sharedir, size_t bufsize)
{
    g_strlcpy(sharedir, darktable.sharedir, bufsize);
}
