#include "common/win_file_trash.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

gboolean dt_win_file_trash(GFile *file, GCancellable *cancellable, GError **error)
{
    if (cancellable && g_cancellable_is_cancelled(cancellable))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "File operation was cancelled");
        return FALSE;
    }

    char *path = g_file_get_path(file);
    if (!path)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Only local files can be moved to the Windows Recycle Bin");
        return FALSE;
    }

    glong wide_length = 0;
    gunichar2 *wide_path = g_utf8_to_utf16(path, -1, NULL, &wide_length, error);
    if (!wide_path)
    {
        g_free(path);
        return FALSE;
    }

    wide_path = g_realloc(wide_path, (wide_length + 2) * sizeof(*wide_path));
    wide_path[wide_length + 1] = L'\0';

    SHFILEOPSTRUCTW operation = { 0 };
    operation.wFunc = FO_DELETE;
    operation.pFrom = wide_path;
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    const int result = SHFileOperationW(&operation);
    g_free(wide_path);
    if (result != 0 || operation.fAnyOperationsAborted)
    {
        g_set_error(error, G_IO_ERROR, operation.fAnyOperationsAborted ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED,
                    "Could not move '%s' to the Windows Recycle Bin", path);
        g_free(path);
        return FALSE;
    }

    g_free(path);
    return TRUE;
}

#endif
