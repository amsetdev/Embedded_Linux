/* Host tests for src/cloud/store_forward.c — what happens to telemetry that
 * could not be published. The ops are fakes: an in-memory directory, a
 * "publish" that records payloads (and can be told the broker is gone), and
 * switches to make listing / reading / removing fail. */

#include <stdlib.h>
#include <string.h>

#include "known_issue.h"
#include "store_forward.h"
#include "unity.h"

#define MAX_FILES 16

typedef struct {
    char names[MAX_FILES][SF_NAME_MAX];
    char data[MAX_FILES][256];
    int  count;
    int  online;
    int  list_fails, read_fails, remove_fails, write_fails;
    int  publish_lost;              /* 1: publish "fails" (mqtt_publish() would store it again) */
    char published[MAX_FILES][256];
    int  publishes, writes;
} fake_t;

static fake_t fk;

static int find(const char *name)
{
    for (int i = 0; i < fk.count; i++)
        if (strcmp(fk.names[i], name) == 0)
            return i;
    return -1;
}

static int f_connected(void *ctx) { (void)ctx; return fk.online; }

static int f_list(void *ctx, void (*each)(const char *, void *), void *each_ctx)
{
    (void)ctx;
    if (fk.list_fails)
        return -1;
    each(".", each_ctx);
    each("..", each_ctx);
    for (int i = 0; i < fk.count; i++)
        each(fk.names[i], each_ctx);
    return 0;
}

static char *f_read(void *ctx, const char *name)
{
    (void)ctx;
    int i = find(name);
    return (i < 0 || fk.read_fails) ? NULL : strdup(fk.data[i]);
}

static int f_write(void *ctx, const char *name, const char *payload)
{
    (void)ctx;
    fk.writes++;
    if (fk.write_fails)
        return -1;
    int i = find(name);
    if (i < 0)
        i = fk.count++;
    strcpy(fk.names[i], name);
    strcpy(fk.data[i], payload);
    return 0;
}

static void f_publish(void *ctx, const char *payload)
{
    (void)ctx;
    if (!fk.publish_lost)
        strcpy(fk.published[fk.publishes++], payload);
}

static int f_remove(void *ctx, const char *name)
{
    (void)ctx;
    int i = find(name);
    if (i < 0 || fk.remove_fails)
        return -1;
    for (int j = i; j < fk.count - 1; j++) {
        strcpy(fk.names[j], fk.names[j + 1]);
        strcpy(fk.data[j], fk.data[j + 1]);
    }
    fk.count--;
    return 0;
}

static const sf_ops_t ops = {
    .ctx = NULL, .connected = f_connected, .list = f_list, .read = f_read,
    .write = f_write, .publish = f_publish, .remove = f_remove,
};

static void add_file(const char *name, const char *data)
{
    strcpy(fk.names[fk.count], name);
    strcpy(fk.data[fk.count], data);
    fk.count++;
}

void setUp(void) { memset(&fk, 0, sizeof(fk)); fk.online = 1; }
void tearDown(void) {}

static void test_store_names_file_by_unix_ms(void)
{
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(0, sf_store(&ops, "{\"ts\":1}", 1767225600, name));
    TEST_ASSERT_EQUAL_STRING("1767225600000.txt", name);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1}", fk.data[0]);
}

static void test_store_null_payload_or_write_failure(void)
{
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(-1, sf_store(&ops, NULL, 1, name));
    TEST_ASSERT_EQUAL_INT(0, fk.writes);
    fk.write_fails = 1;
    TEST_ASSERT_EQUAL_INT(-1, sf_store(&ops, "x", 1, name));
}

static void test_two_stores_in_one_second_both_kept(void)
{
    char a[SF_NAME_MAX], b[SF_NAME_MAX];
    sf_store(&ops, "first", 1767225600, a);
    sf_store(&ops, "second", 1767225600, b);
    KNOWN_ISSUE(fk.count == 2,
                "file names have 1 s resolution (<seconds>000.txt): a second payload stored in the "
                "same second (e.g. a failed replay re-stored next to new telemetry) overwrites the first");
}

static void test_payload_file_names(void)
{
    TEST_ASSERT_TRUE(sf_is_payload_file("1767225600000.txt"));
    TEST_ASSERT_FALSE(sf_is_payload_file("1767225600000.txt.tmp"));
    TEST_ASSERT_FALSE(sf_is_payload_file("notes"));
    TEST_ASSERT_FALSE(sf_is_payload_file("."));
}

static void test_offline_does_nothing(void)
{
    add_file("1000.txt", "a");
    fk.online = 0;
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(SF_NOT_CONNECTED, sf_replay_once(&ops, name));
    TEST_ASSERT_EQUAL_INT(1, fk.count);
    TEST_ASSERT_EQUAL_INT(0, fk.publishes);
}

static void test_nothing_pending_ignores_other_files(void)
{
    add_file("readme.md", "x");
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(SF_NOTHING_PENDING, sf_replay_once(&ops, name));
    TEST_ASSERT_EQUAL_STRING("", name);
}

static void test_oldest_sent_first_then_deleted(void)
{
    add_file("1767225660000.txt", "second");
    add_file("1767225600000.txt", "first");
    add_file("1767225720000.txt", "third");
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(SF_SENT, sf_replay_once(&ops, name));
    TEST_ASSERT_EQUAL_STRING("1767225600000.txt", name);
    TEST_ASSERT_EQUAL_STRING("first", fk.published[0]);
    TEST_ASSERT_EQUAL_INT(-1, find("1767225600000.txt"));

    sf_replay_once(&ops, name);
    sf_replay_once(&ops, name);
    TEST_ASSERT_EQUAL_STRING("second", fk.published[1]);
    TEST_ASSERT_EQUAL_STRING("third", fk.published[2]);
    TEST_ASSERT_EQUAL_INT(SF_NOTHING_PENDING, sf_replay_once(&ops, name));
}

static void test_unreadable_file_kept(void)
{
    add_file("1000.txt", "a");
    fk.read_fails = 1;
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(SF_READ_FAILED, sf_replay_once(&ops, name));
    TEST_ASSERT_EQUAL_INT(1, fk.count);
    TEST_ASSERT_EQUAL_INT(0, fk.publishes);
}

static void test_list_and_remove_failures_reported(void)
{
    char name[SF_NAME_MAX];
    fk.list_fails = 1;
    TEST_ASSERT_EQUAL_INT(SF_LIST_FAILED, sf_replay_once(&ops, name));
    fk.list_fails = 0;
    add_file("1000.txt", "a");
    fk.remove_fails = 1;
    TEST_ASSERT_EQUAL_INT(SF_SENT_NOT_REMOVED, sf_replay_once(&ops, name));
}

static void test_file_kept_when_publish_fails(void)
{
    add_file("1000.txt", "a");
    fk.publish_lost = 1;
    char name[SF_NAME_MAX];
    sf_replay_once(&ops, name);
    KNOWN_ISSUE(find("1000.txt") >= 0,
                "publish has no result (mqtt_publish() is void and QoS 1 isn't waited for): the file "
                "is deleted even when the broker never got it; mqtt_publish() stores a failed "
                "payload again, but as the NEWEST file, so replay order is lost");
}

static void test_backlog_drains(void)
{
    char name[SF_NAME_MAX];
    add_file("1000.txt", "a");
    add_file("2000.txt", "b");
    add_file("3000.txt", "c");
    sf_replay_once(&ops, name);
    KNOWN_ISSUE(fk.count == 0,
                "one stored payload is sent per replay (every 60 s); with a 30 s poll interval new "
                "payloads arrive faster than the backlog drains, so it never empties");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_store_names_file_by_unix_ms);
    RUN_TEST(test_store_null_payload_or_write_failure);
    RUN_TEST(test_two_stores_in_one_second_both_kept);
    RUN_TEST(test_payload_file_names);
    RUN_TEST(test_offline_does_nothing);
    RUN_TEST(test_nothing_pending_ignores_other_files);
    RUN_TEST(test_oldest_sent_first_then_deleted);
    RUN_TEST(test_unreadable_file_kept);
    RUN_TEST(test_list_and_remove_failures_reported);
    RUN_TEST(test_file_kept_when_publish_fails);
    RUN_TEST(test_backlog_drains);
    return UNITY_END();
}
