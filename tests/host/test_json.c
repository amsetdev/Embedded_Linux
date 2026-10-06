/* Host tests for src/util/json.c — the strstr-based reader used for the config
 * file and OTA commands. How the whole config is read is checked against the
 * real parser in tests/validate; these tests pin down the primitives. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "known_issue.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_string_value(void)
{
    char v[32] = "unchanged";
    TEST_ASSERT_EQUAL_INT(0, json_get_string("{\"ssid\": \"plant\"}", "ssid", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("plant", v);
}

static void test_string_after_newline_and_tabs(void)
{
    char v[32];
    TEST_ASSERT_EQUAL_INT(0, json_get_string("{\"ssid\":\n\t \"plant\"}", "ssid", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("plant", v);
}

static void test_missing_key_leaves_value(void)
{
    char v[32] = "default";
    TEST_ASSERT_EQUAL_INT(-1, json_get_string("{\"a\": \"x\"}", "ssid", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("default", v);
}

static void test_non_string_value_rejected(void)
{
    char v[32] = "default";
    TEST_ASSERT_EQUAL_INT(-1, json_get_string("{\"port\": 8883}", "port", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("default", v);
}

static void test_long_string_truncated_to_buffer(void)
{
    char v[6];
    TEST_ASSERT_EQUAL_INT(0, json_get_string("{\"k\": \"abcdefghij\"}", "k", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("abcde", v);
}

static void test_key_is_matched_with_quotes(void)
{
    /* "id" must not match inside "device_id" */
    char v[32] = "none";
    TEST_ASSERT_EQUAL_INT(-1, json_get_string("{\"device_id\": \"GW\"}", "id", v, sizeof(v)));
    TEST_ASSERT_EQUAL_STRING("none", v);
}

static void test_escaped_quote_in_value(void)
{
    char v[32];
    json_get_string("{\"label\": \"TEMP\\\"C\"}", "label", v, sizeof(v));
    KNOWN_ISSUE(strcmp(v, "TEMP\"C") == 0,
                "no unescaping: a string stops at the first '\"' (value read as 'TEMP\\'); "
                "the validate rules forbid '\"' and '\\' in config strings");
}

static void test_int_value(void)
{
    int v = 0;
    TEST_ASSERT_EQUAL_INT(0, json_get_int("{\"baud\": 19200}", "baud", &v));
    TEST_ASSERT_EQUAL_INT(19200, v);
    TEST_ASSERT_EQUAL_INT(0, json_get_int("{\"t\": -5}", "t", &v));
    TEST_ASSERT_EQUAL_INT(-5, v);
}

static void test_int_missing_key(void)
{
    int v = 42;
    TEST_ASSERT_EQUAL_INT(-1, json_get_int("{\"a\": 1}", "baud", &v));
    TEST_ASSERT_EQUAL_INT(42, v);
}

static void test_int_from_string_or_bool_is_zero(void)
{
    /* atoi(): a quoted number or true/false becomes 0, and the call still "succeeds".
     * The validate schema requires real integers for this reason. */
    int v = 7;
    TEST_ASSERT_EQUAL_INT(0, json_get_int("{\"baud\": \"9600\"}", "baud", &v));
    TEST_ASSERT_EQUAL_INT(0, v);
    v = 7;
    json_get_int("{\"enable\": true}", "enable", &v);
    TEST_ASSERT_EQUAL_INT(0, v);
}

static void test_search_continues_past_object_end(void)
{
    /* Documented behaviour the config format depends on (tests/validate): a key
     * missing from one object is found in a later one. */
    int v = 0;
    const char *json = "{\"wifi\": {\"ssid\": \"x\"}, \"modbus_tcp\": {\"enable\": 1}}";
    TEST_ASSERT_EQUAL_INT(0, json_get_int(strstr(json, "\"wifi\""), "enable", &v));
    TEST_ASSERT_EQUAL_INT(1, v);
}

static void test_from_variants_are_the_same(void)
{
    char a[16], b[16];
    int x = 0, y = 0;
    const char *json = "{\"label\": \"L1\", \"address\": 12}";
    json_get_string(json, "label", a, sizeof(a));
    json_get_string_from(json, "label", b, sizeof(b));
    json_get_int(json, "address", &x);
    json_get_int_from(json, "address", &y);
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_EQUAL_INT(x, y);
}

static void test_read_file(void)
{
    char path[] = "/tmp/test_json_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(5, (int)write(fd, "{\"a\"}", 5));
    close(fd);

    char *text = read_file(path);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_STRING("{\"a\"}", text);
    free(text);
    unlink(path);
}

static void test_read_empty_and_missing_file(void)
{
    char path[] = "/tmp/test_json_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    char *text = read_file(path);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_STRING("", text);
    free(text);
    unlink(path);

    TEST_ASSERT_NULL(read_file("/nonexistent/smart_rtu_config.json"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_string_value);
    RUN_TEST(test_string_after_newline_and_tabs);
    RUN_TEST(test_missing_key_leaves_value);
    RUN_TEST(test_non_string_value_rejected);
    RUN_TEST(test_long_string_truncated_to_buffer);
    RUN_TEST(test_key_is_matched_with_quotes);
    RUN_TEST(test_escaped_quote_in_value);
    RUN_TEST(test_int_value);
    RUN_TEST(test_int_missing_key);
    RUN_TEST(test_int_from_string_or_bool_is_zero);
    RUN_TEST(test_search_continues_past_object_end);
    RUN_TEST(test_from_variants_are_the_same);
    RUN_TEST(test_read_file);
    RUN_TEST(test_read_empty_and_missing_file);
    return UNITY_END();
}
