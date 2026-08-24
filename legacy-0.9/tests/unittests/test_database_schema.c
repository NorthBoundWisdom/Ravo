/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "common/database_schema.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

static void test_current_schema_versions(void **state)
{
    assert_int_equal(dt_database_schema_current_version(DT_DATABASE_SCHEMA_LIBRARY), 57);
    assert_int_equal(dt_database_schema_current_version(DT_DATABASE_SCHEMA_DATA), 13);
    assert_string_equal(dt_database_schema_name(DT_DATABASE_SCHEMA_LIBRARY), "library");
    assert_string_equal(dt_database_schema_name(DT_DATABASE_SCHEMA_DATA), "data");
}

static void test_schema_compatibility_is_versioned_independently(void **state)
{
    assert_int_equal(dt_database_schema_check(DT_DATABASE_SCHEMA_LIBRARY, 57),
                     DT_DATABASE_SCHEMA_COMPATIBLE);
    assert_int_equal(dt_database_schema_check(DT_DATABASE_SCHEMA_LIBRARY, 56),
                     DT_DATABASE_SCHEMA_TOO_OLD);
    assert_int_equal(dt_database_schema_check(DT_DATABASE_SCHEMA_LIBRARY, 58),
                     DT_DATABASE_SCHEMA_TOO_NEW);
    assert_int_equal(dt_database_schema_check(DT_DATABASE_SCHEMA_DATA, 13),
                     DT_DATABASE_SCHEMA_COMPATIBLE);
    assert_int_equal(dt_database_schema_check(DT_DATABASE_SCHEMA_DATA, 0),
                     DT_DATABASE_SCHEMA_INVALID);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_current_schema_versions),
        cmocka_unit_test(test_schema_compatibility_is_versioned_independently),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
