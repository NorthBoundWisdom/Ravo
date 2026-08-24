add_cmocka_test(test_sample
                SOURCES test_sample.c
                LINK_LIBRARIES cmocka::cmocka
                LABELS unit)

add_cmocka_test(test_database_schema
                SOURCES test_database_schema.c ../../common/database_schema.c
                LINK_LIBRARIES cmocka::cmocka
                LABELS "unit;database")
