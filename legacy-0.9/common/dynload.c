/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#ifdef HAVE_OPENCL

#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common/dynload.h"
#include "common/darktable.h"

/* check if gmodules is supported on this platform */
gboolean dt_gmodule_supported(void)
{
    return TRUE;
}

/* dynamically load library */
dt_gmodule_t *dt_gmodule_open(const char *library)
{
    // logic here is simplified since it's used only specifically for OpenCL
    dt_gmodule_t *module = NULL;
#ifdef _WIN32
    void *gmodule = LoadLibraryA(library);
#else
    void *gmodule = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
#endif

    if (gmodule != NULL)
    {
        module = malloc(sizeof(dt_gmodule_t));
        module->gmodule = gmodule;
        module->library = g_strdup(library);
    }

    return module;
}

/* get pointer to symbol */
gboolean dt_gmodule_symbol(dt_gmodule_t *module, const char *name, void (**pointer)(void))
{
#ifdef _WIN32
    *pointer = (void (*)(void))GetProcAddress((HMODULE)module->gmodule, name);
#else
    *pointer = dlsym(module->gmodule, name);
#endif
    const gboolean valid = *pointer != NULL;
    if (!valid)
        dt_print(DT_DEBUG_OPENCL, "[opencl init] missing symbol `%s` in library`", name);

    return valid;
}
#endif //HAVE_OPENCL
