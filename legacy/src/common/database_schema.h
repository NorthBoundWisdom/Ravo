/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#pragma once

typedef enum dt_database_schema_t
{
    DT_DATABASE_SCHEMA_LIBRARY,
    DT_DATABASE_SCHEMA_DATA
} dt_database_schema_t;

typedef enum dt_database_schema_compatibility_t
{
    DT_DATABASE_SCHEMA_INVALID,
    DT_DATABASE_SCHEMA_TOO_OLD,
    DT_DATABASE_SCHEMA_COMPATIBLE,
    DT_DATABASE_SCHEMA_TOO_NEW
} dt_database_schema_compatibility_t;

/*
 * These values describe the on-disk schema, not the application release.
 * Do not change them for a DarkTableNext version bump. Increase a value only
 * when its schema changes and add the corresponding migration at the same time.
 */
enum
{
    DT_DATABASE_SCHEMA_VERSION_LIBRARY = 57,
    DT_DATABASE_SCHEMA_VERSION_DATA = 13,

    /* Independent epoch for the idempotent camera-id reconciliation. */
    DT_DATABASE_CAMERA_MAPPING_VERSION = 1
};

int dt_database_schema_current_version(dt_database_schema_t schema);
const char *dt_database_schema_name(dt_database_schema_t schema);
dt_database_schema_compatibility_t dt_database_schema_check(dt_database_schema_t schema,
                                                             int stored_version);
