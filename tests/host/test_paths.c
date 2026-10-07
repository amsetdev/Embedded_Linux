/* Host tests for src/util/paths.c — command line and the configuration / data paths. */

#include <string.h>

#include "paths.h"
#include "unity.h"

void setUp(void) { paths_init(NULL, NULL); }
void tearDown(void) {}

static paths_args_t parse(int argc, const char *const *argv)
{
    return paths_parse_args(argc, (char **)argv);
}

static void test_defaults_are_legacy_working_directory(void)
{
    const char *argv[] = { "gateway" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_RUN, parse(1, argv));
    TEST_ASSERT_EQUAL_STRING("smart_rtu_config.json", paths_config());
    char buf[PATHS_MAX];
    TEST_ASSERT_EQUAL_STRING("./storage", paths_data("storage", buf, sizeof(buf)));
}

static void test_production_options(void)
{
    const char *argv[] = { "gateway", "--config", "/etc/gateway/smart_rtu_config.json",
                           "--data-dir", "/var/lib/gateway/" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_RUN, parse(5, argv));
    TEST_ASSERT_EQUAL_STRING("/etc/gateway/smart_rtu_config.json", paths_config());
    TEST_ASSERT_EQUAL_STRING("/var/lib/gateway", paths_data_dir());
    char buf[PATHS_MAX];
    TEST_ASSERT_EQUAL_STRING("/var/lib/gateway/storage", paths_data("storage", buf, sizeof(buf)));
}

static void test_short_and_equals_forms(void)
{
    const char *a[] = { "gateway", "-c", "/a.json", "-d", "/d" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_RUN, parse(5, a));
    TEST_ASSERT_EQUAL_STRING("/a.json", paths_config());
    const char *b[] = { "gateway", "--config=/b.json", "--data-dir=/" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_RUN, parse(3, b));
    char buf[PATHS_MAX];
    TEST_ASSERT_EQUAL_STRING("/ota", paths_data("ota", buf, sizeof(buf)));
}

static void test_version_and_help(void)
{
    const char *v[] = { "gateway", "--config", "/x", "--version" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_VERSION, parse(4, v));
    const char *h[] = { "gateway", "-h" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_HELP, parse(2, h));
}

static void test_bad_arguments_change_nothing(void)
{
    const char *missing[] = { "gateway", "--config" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_ERROR, parse(2, missing));
    const char *unknown[] = { "gateway", "--verbose" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_ERROR, parse(2, unknown));
    const char *empty[] = { "gateway", "--config=" };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_ERROR, parse(2, empty));
    static char longpath[PATHS_MAX + 10];
    memset(longpath, 'a', sizeof(longpath) - 1);
    const char *toolong[] = { "gateway", "--data-dir", longpath };
    TEST_ASSERT_EQUAL_INT(PATHS_ARGS_ERROR, parse(3, toolong));
    TEST_ASSERT_EQUAL_STRING("smart_rtu_config.json", paths_config());
    TEST_ASSERT_EQUAL_STRING(".", paths_data_dir());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_legacy_working_directory);
    RUN_TEST(test_production_options);
    RUN_TEST(test_short_and_equals_forms);
    RUN_TEST(test_version_and_help);
    RUN_TEST(test_bad_arguments_change_nothing);
    return UNITY_END();
}
