/* Host tests for src/cloud/config_push.c — a configuration pushed by the tool
 * over MQTT: what is accepted, when the file is written, when the application
 * reloads, and the ack the tool waits for. */

#include <stdlib.h>
#include <string.h>

#include "config_push.h"
#include "unity.h"

typedef struct {
    char current[512];
    int  has_current;
    int  write_fails;
    int  writes, reloads;
    char written[512];
} fake_t;

static fake_t fk;

static char *f_read(void *ctx) { (void)ctx; return fk.has_current ? strdup(fk.current) : NULL; }

static int f_write(void *ctx, const char *content, size_t len)
{
    (void)ctx;
    fk.writes++;
    if (fk.write_fails)
        return -1;
    memcpy(fk.written, content, len);
    fk.written[len] = '\0';
    return 0;
}

static void f_reload(void *ctx) { (void)ctx; fk.reloads++; }

static const cp_ops_t ops = { NULL, f_read, f_write, f_reload };

static const char *CONFIG =
    "{\"device\": {\"device_id\": \"GW-1\"}, \"wifi\": {\"ssid\": \"x\"},\n"
    " \"registers\": [{\"label\": \"A}\", \"address\": 0}, {\"label\": \"B\", \"address\": 1}]}\n";

static char ack[128];

void setUp(void) { memset(&fk, 0, sizeof(fk)); ack[0] = '\0'; }
void tearDown(void) {}

static cp_result_t push(const char *payload)
{
    return config_push_apply(&ops, payload, strlen(payload), ack, sizeof(ack));
}

static void test_new_config_saved_and_reload_requested(void)
{
    TEST_ASSERT_EQUAL_INT(CP_SAVED, push(CONFIG));
    TEST_ASSERT_EQUAL_STRING(CONFIG, fk.written);
    TEST_ASSERT_EQUAL_INT(1, fk.reloads);
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"saved\",\"registers\":2}", ack);
}

static void test_same_config_not_rewritten(void)
{
    /* The retained message comes back on every reconnect: no write, no reload loop. */
    strcpy(fk.current, CONFIG);
    fk.has_current = 1;
    TEST_ASSERT_EQUAL_INT(CP_UNCHANGED, push(CONFIG));
    TEST_ASSERT_EQUAL_INT(0, fk.writes);
    TEST_ASSERT_EQUAL_INT(0, fk.reloads);
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"unchanged\",\"registers\":2}", ack);
}

static void test_empty_register_list_accepted(void)
{
    TEST_ASSERT_EQUAL_INT(CP_SAVED, push("{\"device\": {}, \"registers\": []}"));
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"saved\",\"registers\":0}", ack);
}

static void test_not_a_config_rejected_nothing_written(void)
{
    const char *bad[] = {
        "",                                                    /* empty: also "clear retained" */
        "hello",
        "[1, 2]",
        "{\"device\": {}}",                                    /* no registers */
        "{\"registers\": []}",                                 /* no device object */
        "{\"device\": {}, \"registers\": [}",                  /* broken */
        "{\"device\": {}, \"registers\": []} trailing",
        "{\"device\": \"x\", \"registers\": []}",
    };
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(CP_REJECTED, push(bad[i]), bad[i]);
        TEST_ASSERT_NOT_NULL(strstr(ack, "\"status\":\"rejected\""));
    }
    TEST_ASSERT_EQUAL_INT(0, fk.writes);
    TEST_ASSERT_EQUAL_INT(0, fk.reloads);
}

static void test_payload_with_nul_or_too_large_rejected(void)
{
    char with_nul[] = "{\"device\": {}, \"registers\": []}";
    with_nul[5] = '\0';
    TEST_ASSERT_EQUAL_INT(CP_REJECTED, config_push_apply(&ops, with_nul, sizeof(with_nul) - 1, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_INT(CP_REJECTED, config_push_apply(&ops, CONFIG, CONFIG_PUSH_MAX + 1, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"rejected\",\"error\":\"size\"}", ack);
}

static void test_payload_not_nul_terminated(void)
{
    char buf[256];
    size_t n = strlen(CONFIG);
    memcpy(buf, CONFIG, n);
    memset(buf + n, 'x', sizeof(buf) - n);      /* garbage after the payload */
    TEST_ASSERT_EQUAL_INT(CP_SAVED, config_push_apply(&ops, buf, n, ack, sizeof(ack)));
    TEST_ASSERT_EQUAL_STRING(CONFIG, fk.written);
}

static void test_write_failure_keeps_old_config(void)
{
    fk.write_fails = 1;
    TEST_ASSERT_EQUAL_INT(CP_WRITE_FAILED, push(CONFIG));
    TEST_ASSERT_EQUAL_INT(0, fk.reloads);
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"error\",\"error\":\"write_failed\"}", ack);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_new_config_saved_and_reload_requested);
    RUN_TEST(test_same_config_not_rewritten);
    RUN_TEST(test_empty_register_list_accepted);
    RUN_TEST(test_not_a_config_rejected_nothing_written);
    RUN_TEST(test_payload_with_nul_or_too_large_rejected);
    RUN_TEST(test_payload_not_nul_terminated);
    RUN_TEST(test_write_failure_keeps_old_config);
    return UNITY_END();
}
