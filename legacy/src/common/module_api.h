/*
    This file is part of darktable,
    Copyright (C) 2016-2021 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <glib.h>

#undef DT_MODULE_API_OPTIONAL
#undef DT_MODULE_API_REQUIRED
#undef DT_MODULE_API_DEFAULT

#undef FULL_API_H

#ifdef INCLUDE_API_FROM_MODULE_LOAD
#define DT_MODULE_API_OPTIONAL(return_type, function_name, ...)                                                  \
    if (!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name)))    \
    module->function_name = NULL
#define DT_MODULE_API_REQUIRED(return_type, function_name, ...)                                                  \
    if (!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name)))    \
    goto api_h_error
#define DT_MODULE_API_DEFAULT(return_type, function_name, ...)                                                   \
    if (!g_module_symbol(module->module, #function_name, (gpointer) & (module->function_name)))    \
    module->function_name = default_##function_name

dt_print(DT_DEBUG_CONTROL, "[" INCLUDE_API_FROM_MODULE_LOAD "] loading `%s' from %s", module_name,
         libname);
module->module = g_module_open(libname, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
if (!module->module)
    goto api_h_error;
int (*version)();
if (!g_module_symbol(module->module, "dt_module_dt_version", (gpointer) & (version)))
    goto api_h_error;
if (version() != dt_version())
{
    dt_print(DT_DEBUG_ALWAYS,
             "[" INCLUDE_API_FROM_MODULE_LOAD
             "] `%s' is compiled for another version of dt (module %d (%s) != dt %d (%s)) !",
             libname, abs(version()), version() < 0 ? "debug" : "opt", abs(dt_version()),
             dt_version() < 0 ? "debug" : "opt");
    goto api_h_error;
}
if (!g_module_symbol(module->module, "dt_module_mod_version", (gpointer) & (module->version)))
    goto api_h_error;

goto skip_error;
api_h_error
    : dt_print(DT_DEBUG_ALWAYS, "[" INCLUDE_API_FROM_MODULE_LOAD "] failed to open `%s': %s",
               module_name, g_module_error());
if (module->module)
    g_module_close(module->module);
module->module = NULL;
return 1;
skip_error:
#undef INCLUDE_API_FROM_MODULE_LOAD
#elif defined(INCLUDE_API_FROM_MODULE_H)
#define DT_MODULE_API_OPTIONAL(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
#define DT_MODULE_API_REQUIRED(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
#define DT_MODULE_API_DEFAULT(return_type, function_name, ...) return_type (*function_name)(__VA_ARGS__)
int (*version)();
#undef INCLUDE_API_FROM_MODULE_H
#elif defined(INCLUDE_API_FROM_MODULE_LOAD_BY_SO)
#define DT_MODULE_API_OPTIONAL(return_type, function_name, ...) module->function_name = so->function_name
#define DT_MODULE_API_REQUIRED(return_type, function_name, ...) module->function_name = so->function_name
#define DT_MODULE_API_DEFAULT(return_type, function_name, ...) module->function_name = so->function_name
#undef INCLUDE_API_FROM_MODULE_LOAD_BY_SO
#else
#define FULL_API_H
#define DT_MODULE_API_OPTIONAL(return_type, function_name, ...) return_type function_name(__VA_ARGS__)
#define DT_MODULE_API_REQUIRED(return_type, function_name, ...) return_type function_name(__VA_ARGS__)
#define DT_MODULE_API_DEFAULT(return_type, function_name, ...) return_type function_name(__VA_ARGS__)
G_BEGIN_DECLS
// these 2 functions are defined by DT_MODULE() macro.
#if defined(__GNUC__)
#pragma GCC visibility push(default)
#endif
// returns the version of dt's module interface at the time this module was build
int dt_module_dt_version(void);
// returns the version of this module
int dt_module_mod_version(void);
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
G_END_DECLS
#endif
