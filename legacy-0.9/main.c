/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#include "common/darktable.h"
#include "common/image.h"
#include "develop/develop.h"
#include "common/gimp.h"
#include "common/image_cache.h"
#include "gui/gtk.h"
#include <stdlib.h>

#include "osx/osx.h"

int main(int argc, char *argv[])
{
#ifdef __APPLE__
    dt_osx_prepare_environment();
#endif

    if (dt_init(argc, argv, TRUE, TRUE))
    {
        if (dt_gimpmode())
            printf("\n<<<gimp\nerror\ngimp>>>\n");
        exit(1);
    }

    if (dt_check_gimpmode_ok("version"))
    {
        printf("\n<<<gimp\n%d\ngimp>>>\n", DT_GIMP_VERSION);
        exit(0);
    }

    if (dt_check_gimpmode("version") ||
        (dt_check_gimpmode("file") && !dt_check_gimpmode_ok("file")) ||
        (dt_check_gimpmode("thumb") && !dt_check_gimpmode_ok("thumb")) || darktable.gimp.error)
    {
        printf("\n<<<gimp\nerror\ngimp>>>\n");
        exit(1);
    }

    if (dt_check_gimpmode_ok("file"))
    {
        const dt_imgid_t id = dt_gimp_load_darkroom(darktable.gimp.path);
        if (!dt_is_valid_imgid(id))
            darktable.gimp.error = TRUE;
    }

    if (dt_check_gimpmode_ok("thumb"))
    {
        const dt_imgid_t id = dt_gimp_load_image(darktable.gimp.path);
        if (dt_is_valid_imgid(id))
            darktable.gimp.error = !dt_export_gimp_file(id);
        else
            darktable.gimp.error = TRUE;

        return darktable.gimp.error ? 1 : 0;
    }

    if (!dt_gimpmode() || dt_check_gimpmode_ok("file"))
        dt_gui_gtk_run(darktable.gui);

    dt_cleanup();

    if (dt_gimpmode() && darktable.gimp.error)
        printf("\n<<<gimp\nerror\ngimp>>>\n");

    const int exitcode = dt_gimpmode() ? (darktable.gimp.error ? 1 : 0) : 0;
    exit(exitcode);
}
