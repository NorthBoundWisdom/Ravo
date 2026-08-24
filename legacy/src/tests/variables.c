#include "common/darktable.h"
#include "common/variables.h"

#include <stdio.h>

typedef struct test_case_t
{
    char *input, *expected_result;
} test_case_t;

typedef struct test_t
{
    char *filename, *jobcode, sequence;
    test_case_t test_cases[];
} test_t;

int run_test(const test_t *test, int *n_tests, int *n_failed)
{
    dt_variables_params_t *params;
    dt_variables_params_init(&params);
    params->filename = test->filename; //"abcdef12345abcdef";
    params->jobcode = test->jobcode;   //"ABCDEF12345ABCDEF";
    params->sequence = test->sequence;

    *n_failed = 0;
    *n_tests = 0;
    for (const test_case_t *test_case = test->test_cases; test_case->input; test_case++)
    {
        (*n_tests)++;
        char *result = dt_variables_expand(params, test_case->input, FALSE);
        if (g_strcmp0(result, test_case->expected_result))
        {
            (*n_failed)++;
            printf("  [FAIL] input: '%s', result: '%s', expected: '%s'\n", test_case->input, result,
                   test_case->expected_result);
        }
        else
            printf("  [OK] input: '%s', result: '%s'\n", test_case->input, result);
        g_free(result);
    }

    dt_variables_params_destroy(params);

    return *n_failed > 0 ? 1 : 0;
}

static const test_t test_variables = {"abcdef12345abcdef",
                                      "ABCDEF12345ABCDEF",
                                      23,
                                      {{"$(FILE.NAME)", "abcdef12345abcdef"},
                                       {"foo-$(FILE.NAME)-bar", "foo-abcdef12345abcdef-bar"},
                                       {"äöü-$(FILE.NAME)-äöü", "äöü-abcdef12345abcdef-äöü"},
                                       {"$(FILE.NAME).$(SEQUENCE)", "abcdef12345abcdef.0023"},
                                       {"$(NONEXISTANT)", ""},
                                       {"foo-$(NONEXISTANT)-bar", "foo--bar"},

                                       {NULL, NULL}}};

static const test_t test_simple_substitutions = {
    "abcdef12345abcdef",
    "ABCDEF12345ABCDEF",
    23,
    {{"$(NONEXISTANT-invälid)", "invälid"},
     {"$(FILE.NAME-invälid)", "abcdef12345abcdef"},

     {"$(NONEXISTANT+exißts)", ""},
     {"$(FILE.NAME+exißts)", "exißts"},

     {"$(NONEXISTANT:0)", ""},
     {"$(FILE.NAME:0)", "abcdef12345abcdef"},
     {"$(FILE.NAME:5)", "f12345abcdef"},
     {"$(FILE.NAME:42)", ""},
     {"$(FILE.NAME:-5)", "bcdef"},
     {"$(FILE.NAME:-42)", "abcdef12345abcdef"},
     {"$(FILE.NAME:0:5)", "abcde"},
     {"$(FILE.NAME:5:3)", "f12"},
     {"$(FILE.NAME:5:42)", "f12345abcdef"},
     {"$(FILE.NAME:-5:3)", "bcd"},
     {"$(FILE.NAME:-7:-2)", "5abcd"},
     {"$(FILE.NAME:)", "abcdef12345abcdef"},
     {"$(FILE.NAME:5:)", ""},

     {"$(NONEXISTANT#abc)", ""},
     {"$(FILE.NAME#abc)", "def12345abcdef"},
     {"$(FILE.NAME#def)", "abcdef12345abcdef"},

     {"$(NONEXISTANT%abc)", ""},
     {"$(FILE.NAME%abc)", "abcdef12345abcdef"},
     {"$(FILE.NAME%def)", "abcdef12345abc"},

     {"$(NONEXISTANT/abc/def)", ""},
     {"$(FILE.NAME/abc/foobar)", "foobardef12345abcdef"},
     {"$(FILE.NAME/def/foobar)", "abcfoobar12345abcdef"},
     {"$(FILE.NAME//abc/foobar)", "foobardef12345foobardef"},
     {"$(FILE.NAME//def/foobar)", "abcfoobar12345abcfoobar"},
     {"$(FILE.NAME/#abc/foobar)", "foobardef12345abcdef"},
     {"$(FILE.NAME/#def/foobar)", "abcdef12345abcdef"},
     {"$(FILE.NAME/%abc/foobar)", "abcdef12345abcdef"},
     {"$(FILE.NAME/%def/foobar)", "abcdef12345abcfoobar"},

     {"$(NONEXISTANT^)", ""},
     {"$(NONEXISTANT^^)", ""},
     {"$(FILE.NAME^)", "Abcdef12345abcdef"},
     {"$(FILE.NAME^^)", "ABCDEF12345ABCDEF"},

     {"$(NONEXISTANT,)", ""},
     {"$(NONEXISTANT,,)", ""},
     {"$(JOBCODE,)", "aBCDEF12345ABCDEF"},
     {"$(JOBCODE,,)", "abcdef12345abcdef"},

     {NULL, NULL}}};

static const test_t test_recursive_substitutions = {
    "abcdef12345abcdef",
    "ABCDEF12345ABCDEF",
    23,
    {{"x$(TITLE-$(FILE.NAME))y", "xabcdef12345abcdefy"},
     {"x$(TITLE-a-$(FILE.NAME)-b)y", "xa-abcdef12345abcdef-by"},
     {"x$(SEQUENCE-$(FILE.NAME))y", "x0023y"},
     {"x$(FILE.NAME/12345/$(SEQUENCE))y", "xabcdef0023abcdefy"},
     {"x$(FILE.NAME/12345/.$(SEQUENCE).)y", "xabcdef.0023.abcdefy"},

     {NULL, NULL}}};

static const test_t test_broken_variables = {
    "abcdef12345abcdef",
    "ABCDEF12345ABCDEF",
    23,
    {{"$(NONEXISTANT", "$(NONEXISTANT"},
     {"x(NONEXISTANT23", "x(NONEXISTANT23"},
     {"$(FILE.NAME", "$(FILE.NAME"},
     {"x$(FILE.NAME", "x$(FILE.NAME"},
     {"x$(TITLE-$(FILE.NAME)", "x$(TITLE-abcdef12345abcdef"},

     {NULL, NULL}}};

static const test_t test_escapes = {"/home/test/Images/IMG_0123.CR2",
                                    "/home/test/",
                                    23,
                                    {{"foobarbaz", "foobarbaz"},
                                     {"foo/bar/baz", "foo/bar/baz"},
                                     {"foo\\bar\\baz", "foobarbaz"},
                                     {"foo\\\\bar\\\\baz", "foo\\bar\\baz"},
                                     {"foo\\$(bar", "foo$(bar"},
                                     {"foo$\\(bar", "foo$(bar"},
                                     {"foo\\$\\(bar", "foo$(bar"},
                                     {"foo\\$(bar$(SEQUENCE)baz", "foo$(bar0023baz"},
                                     {"foo$(bar$(SEQUENCE)baz", "foo$(bar0023baz"},
                                     {"$(FILE.FOLDER)/darktable_exported/img_$(SEQUENCE)",
                                      "/home/test/Images/darktable_exported/img_0023"},
                                     {"$(FILE.FOLDER)/darktable_exported/$(FILE.NAME)",
                                      "/home/test/Images/darktable_exported/IMG_0123"},

                                     {NULL, NULL}}};

static const test_t test_real_paths = {
    "/home/test/Images/0023/IMG_0123.CR2",
    "/home/test",
    23,
    {{"$(FILE.FOLDER#$(JOBCODE))", "/Images/0023"},
     {"$(FILE.FOLDER#$(JOBCODE)/Images)", "/0023"},

     {"$(FILE.FOLDER%$(SEQUENCE))", "/home/test/Images/"},
     {"$(FILE.FOLDER%/$(SEQUENCE))", "/home/test/Images"},

     {"$(FILE.FOLDER/test/$(SEQUENCE))", "/home/0023/Images/0023"},
     {"$(FILE.FOLDER/test/$(SEQUENCE)-$(SEQUENCE))", "/home/0023-0023/Images/0023"},
     {"$(FILE.FOLDER/test/$(SEQUENCE//0/o))", "/home/oo23/Images/0023"},
     {"$(FILE.FOLDER/$(SEQUENCE)/XXX)", "/home/test/Images/XXX"},
     {"$(FILE.FOLDER/$(JOBCODE)\\///media/)", "/media/Images/0023"},
     {"$(FILE.FOLDER/\\/home\\/test\\///media/exports/)/darktable_exported/img_$(SEQUENCE)",
      "/media/exports/Images/0023/darktable_exported/img_0023"},

     {"$(FILE.FOLDER/", "$(FILE.FOLDER/"},
     {"$(FILE.FOLDER/home", "$(FILE.FOLDER/home"},
     {"$(FILE.FOLDER/home/media", "$(FILE.FOLDER/home/media"},
     {"$(FILE.FOLDER/home/media)", "/media/test/Images/0023"},

     {NULL, NULL}}};

#define TEST(t)                                                                                    \
    {                                                                                              \
        int n_failed = 0, n_tests = 0;                                                             \
        n_test_functions++;                                                                        \
        printf("running test '" #t "'\n");                                                         \
        n_test_functions_failed += run_test(&t, &n_tests, &n_failed);                              \
        n_tests_overall += n_tests;                                                                \
        n_failed_overall += n_failed;                                                              \
        printf("%d / %d tests failed\n\n", n_failed, n_tests);                                     \
    }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (dt_pthread_mutex_init(&darktable.metadata_threadsafe, NULL))
    {
        fprintf(stderr, "failed to initialize metadata mutex\n");
        return 1;
    }

    int n_tests_overall = 0, n_failed_overall = 0, n_test_functions = 0,
        n_test_functions_failed = 0;

    TEST(test_variables)

    TEST(test_simple_substitutions)

    TEST(test_recursive_substitutions)

    TEST(test_broken_variables)

    TEST(test_escapes)

    TEST(test_real_paths)

    printf("%d / %d tests failed (%d / %d)\n", n_failed_overall, n_tests_overall,
           n_test_functions_failed, n_test_functions);

    dt_pthread_mutex_destroy(&darktable.metadata_threadsafe);

    return n_failed_overall > 0 ? 1 : 0;
}
