/* Host tests for src/cloud/store_forward.c — what happens to telemetry that
 * could not be published. The ops are fakes: an in-memory directory, a
 * "publish" that records payloads (and can be told the broker doesn't confirm),
 * and switches to make listing / reading / writing / removing fail. */

#include <stdlib.h>
#include <string.h>

#include "store_forward.h"
#include "unity.h"

#define MAX_FILES 160

typedef struct {
    char names[MAX_FILES][SF_NAME_MAX];
    char data[MAX_FILES][64];
    int  count;
    int  online;
    int  list_fails, read_fails, remove_fails, write_fails;
    int  unconfirmed;               /* 1: the broker never confirms (publish returns -1) */
    char published[MAX_FILES][64];
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
    if (fk.write_fails || find(name) >= 0)      /* never overwrite, like fopen("wx") */
        return -1;
    strcpy(fk.names[fk.count], name);
    strcpy(fk.data[fk.count], payload);
    fk.count++;
    return 0;
}

static int f_publish(void *ctx, const char *payload)
{
    (void)ctx;
    if (fk.unconfirmed)
        return -1;
    strcpy(fk.published[fk.publishes++], payload);
    return 0;
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

static void test_store_name_is_ms_and_sequence(void)
{
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(0, sf_store(&ops, "{\"ts\":1}", 1767225600123LL, name));
    TEST_ASSERT_EQUAL_INT(24, (int)strlen(name));                 /* 13 + '_' + 6 + ".txt" */
    TEST_ASSERT_EQUAL_STRING_LEN("1767225600123_", name, 14);
    TEST_ASSERT_EQUAL_STRING(".txt", name + 20);
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
    /* Regression: names had 1 s resolution, so a second payload in the same second
     * overwrote the first. */
    char a[SF_NAME_MAX], b[SF_NAME_MAX];
    sf_store(&ops, "first", 1767225600000LL, a);
    sf_store(&ops, "second", 1767225600000LL, b);
    TEST_ASSERT_EQUAL_INT(2, fk.count);
    TEST_ASSERT_TRUE(strcmp(a, b) < 0);                          /* storing order = replay order */
}

static void test_new_names_sort_after_old_firmware_names(void)
{
    /* Files written by older firmware ("<ms>.txt") are replayed first. */
    char name[SF_NAME_MAX];
    add_file("1767225600000.txt", "old");
    sf_store(&ops, "new", 1767225600000LL, name);
    sf_replay_once(&ops, name);
    TEST_ASSERT_EQUAL_STRING("old", fk.published[0]);
}

static void test_payload_file_names(void)
{
    TEST_ASSERT_TRUE(sf_is_payload_file("1767225600000.txt"));
    TEST_ASSERT_TRUE(sf_is_payload_file("1767225600000_000001.txt"));
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
    add_file("1767225660000_000000.txt", "second");
    add_file("1767225600000_000000.txt", "first");
    add_file("1767225720000_000000.txt", "third");
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(SF_SENT, sf_replay_once(&ops, name));
    TEST_ASSERT_EQUAL_STRING("1767225600000_000000.txt", name);
    TEST_ASSERT_EQUAL_STRING("first", fk.published[0]);
    TEST_ASSERT_EQUAL_INT(-1, find("1767225600000_000000.txt"));

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

static void test_file_kept_when_publish_not_confirmed(void)
{
    /* Regression: the file was deleted even when the broker never got the payload
     * (and mqtt_publish() re-stored it as the newest file, losing the order). */
    add_file("1000.txt", "a");
    fk.unconfirmed = 1;
    char name[SF_NAME_MAX];
    TEST_ASSERT_EQUAL_INT(SF_PUBLISH_FAILED, sf_replay_once(&ops, name));
    TEST_ASSERT_TRUE(find("1000.txt") >= 0);
    TEST_ASSERT_EQUAL_INT(0, fk.writes);                         /* not stored a second time */
}

static void test_backlog_drains(void)
{
    /* Regression: one payload was sent per 60 s round, slower than a 30 s poll
     * interval produces them, so a backlog never drained. */
    char name[SF_NAME_MAX];
    sf_replay_result_t last;
    for (int i = 0; i < 3; i++)
        add_file(i == 0 ? "1000.txt" : i == 1 ? "2000.txt" : "3000.txt", "x");
    TEST_ASSERT_EQUAL_INT(3, sf_replay(&ops, SF_REPLAY_BATCH, &last, name));
    TEST_ASSERT_EQUAL_INT(0, fk.count);
    TEST_ASSERT_EQUAL_INT(SF_NOTHING_PENDING, last);
}

static void test_round_limited_to_batch_size(void)
{
    char name[SF_NAME_MAX], n[SF_NAME_MAX];
    sf_replay_result_t last;
    for (int i = 0; i < SF_REPLAY_BATCH + 5; i++)
        sf_store(&ops, "x", 1000 + i, n);
    TEST_ASSERT_EQUAL_INT(SF_REPLAY_BATCH, sf_replay(&ops, SF_REPLAY_BATCH, &last, name));
    TEST_ASSERT_EQUAL_INT(5, fk.count);
    TEST_ASSERT_EQUAL_INT(SF_SENT, last);
}

static void test_round_stops_at_first_unconfirmed_publish(void)
{
    char name[SF_NAME_MAX];
    sf_replay_result_t last;
    add_file("1000.txt", "a");
    add_file("2000.txt", "b");
    fk.unconfirmed = 1;
    TEST_ASSERT_EQUAL_INT(0, sf_replay(&ops, SF_REPLAY_BATCH, &last, name));
    TEST_ASSERT_EQUAL_INT(SF_PUBLISH_FAILED, last);
    TEST_ASSERT_EQUAL_INT(2, fk.count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_store_name_is_ms_and_sequence);
    RUN_TEST(test_store_null_payload_or_write_failure);
    RUN_TEST(test_two_stores_in_one_second_both_kept);
    RUN_TEST(test_new_names_sort_after_old_firmware_names);
    RUN_TEST(test_payload_file_names);
    RUN_TEST(test_offline_does_nothing);
    RUN_TEST(test_nothing_pending_ignores_other_files);
    RUN_TEST(test_oldest_sent_first_then_deleted);
    RUN_TEST(test_unreadable_file_kept);
    RUN_TEST(test_list_and_remove_failures_reported);
    RUN_TEST(test_file_kept_when_publish_not_confirmed);
    RUN_TEST(test_backlog_drains);
    RUN_TEST(test_round_limited_to_batch_size);
    RUN_TEST(test_round_stops_at_first_unconfirmed_publish);
    return UNITY_END();
}
