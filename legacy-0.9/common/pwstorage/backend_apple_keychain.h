// This file is part of darktable
// Copyright (C) 2024 darktable developers.

// darktable is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// darktable is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with darktable.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <glib.h>

/** apple keychain backend context */
typedef struct backend_apple_keychain_context_t
{
} backend_apple_keychain_context_t;

/** Initializes a new apple keychain backend context. */
const backend_apple_keychain_context_t *dt_pwstorage_apple_keychain_new();

/** Cleanup and destroy apple keychain backend context. */
void dt_pwstorage_apple_keychain_destroy(const backend_apple_keychain_context_t *context);

/** Store (key,value) pairs. */
gboolean dt_pwstorage_apple_keychain_set(const backend_apple_keychain_context_t *context,
                                         const gchar *slot, GHashTable *table);

/** Load (key,value) pairs. */
GHashTable *dt_pwstorage_apple_keychain_get(const backend_apple_keychain_context_t *context,
                                            const gchar *slot);
