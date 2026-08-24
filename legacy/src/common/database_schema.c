/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "common/database_schema.h"

int dt_database_schema_current_version(const dt_database_schema_t schema)
{
    switch (schema)
    {
        case DT_DATABASE_SCHEMA_LIBRARY:
            return DT_DATABASE_SCHEMA_VERSION_LIBRARY;
        case DT_DATABASE_SCHEMA_DATA:
            return DT_DATABASE_SCHEMA_VERSION_DATA;
    }

    return 0;
}

const char *dt_database_schema_name(const dt_database_schema_t schema)
{
    switch (schema)
    {
        case DT_DATABASE_SCHEMA_LIBRARY:
            return "library";
        case DT_DATABASE_SCHEMA_DATA:
            return "data";
    }

    return "unknown";
}

dt_database_schema_compatibility_t dt_database_schema_check(const dt_database_schema_t schema,
                                                             const int stored_version)
{
    const int current_version = dt_database_schema_current_version(schema);
    if (current_version <= 0 || stored_version <= 0)
        return DT_DATABASE_SCHEMA_INVALID;
    if (stored_version < current_version)
        return DT_DATABASE_SCHEMA_TOO_OLD;
    if (stored_version > current_version)
        return DT_DATABASE_SCHEMA_TOO_NEW;
    return DT_DATABASE_SCHEMA_COMPATIBLE;
}
