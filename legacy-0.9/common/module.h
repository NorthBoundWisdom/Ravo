/*
 *    This file is part of darktable,
 *    Copyright (C) 2017-2020 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib.h>

GList *dt_module_load_modules(const char *subdir, const size_t module_size,
                              int (*load_module_so)(void *module, const char *libname,
                                                    const char *plugin_name),
                              void (*init_module)(void *module),
                              gint (*sort_modules)(gconstpointer a, gconstpointer b));
