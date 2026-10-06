/* Host tests for src/util/msg_queue.c — the queue between the Modbus thread
 * (producer) and the MQTT publisher thread (consumer). */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "msg_queue.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void pop_expect(msg_queue_t *q, const char *want)
{
    char *m = msg_queue_pop(q);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_STRING(want, m);
    free(m);
}

static void test_fifo_order(void)
{
    msg_queue_t *q = msg_queue_create(4);
    msg_queue_push(q, "a");
    msg_queue_push(q, "b");
    msg_queue_push(q, "c");
    pop_expect(q, "a");
    pop_expect(q, "b");
    pop_expect(q, "c");
    msg_queue_destroy(q);
}

static void test_push_copies_payload(void)
{
    msg_queue_t *q = msg_queue_create(2);
    char buf[8] = "first";
    msg_queue_push(q, buf);
    strcpy(buf, "changed");
    pop_expect(q, "first");
    msg_queue_destroy(q);
}

static void test_full_queue_drops_oldest(void)
{
    msg_queue_t *q = msg_queue_create(3);
    for (const char *s = "12345"; *s; s++) {
        char m[2] = { *s, 0 };
        TEST_ASSERT_EQUAL_INT(0, msg_queue_push(q, m));
    }
    pop_expect(q, "3");
    pop_expect(q, "4");
    pop_expect(q, "5");
    msg_queue_destroy(q);
}

static void test_wraps_around_many_times(void)
{
    msg_queue_t *q = msg_queue_create(3);
    char m[16];
    for (int i = 0; i < 100; i++) {
        snprintf(m, sizeof(m), "%d", i);
        msg_queue_push(q, m);
        pop_expect(q, m);
    }
    msg_queue_destroy(q);
}

static void test_invalid_arguments(void)
{
    msg_queue_t *q = msg_queue_create(0);   /* 0 -> default capacity */
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_INT(-1, msg_queue_push(q, NULL));
    TEST_ASSERT_EQUAL_INT(-1, msg_queue_push(NULL, "x"));
    TEST_ASSERT_NULL(msg_queue_pop(NULL));
    msg_queue_destroy(q);
}

static void test_items_still_delivered_after_shutdown(void)
{
    msg_queue_t *q = msg_queue_create(4);
    msg_queue_push(q, "last");
    msg_queue_shutdown(q);
    pop_expect(q, "last");
    TEST_ASSERT_NULL(msg_queue_pop(q));
    msg_queue_destroy(q);
}

static void *consumer(void *arg)
{
    return msg_queue_pop(arg);
}

static void test_shutdown_wakes_blocked_consumer(void)
{
    msg_queue_t *q = msg_queue_create(4);
    pthread_t t;
    pthread_create(&t, NULL, consumer, q);
    usleep(50000);                       /* let it block in pop() */
    msg_queue_shutdown(q);
    void *got = (void *)1;
    pthread_join(t, &got);
    TEST_ASSERT_NULL(got);
    msg_queue_destroy(q);
}

static void test_push_wakes_blocked_consumer(void)
{
    msg_queue_t *q = msg_queue_create(4);
    pthread_t t;
    pthread_create(&t, NULL, consumer, q);
    usleep(50000);
    msg_queue_push(q, "hello");
    void *got = NULL;
    pthread_join(t, &got);
    TEST_ASSERT_EQUAL_STRING("hello", (char *)got);
    free(got);
    msg_queue_destroy(q);
}

static void test_destroy_frees_pending(void)
{
    /* LeakSanitizer reports the queued copies if destroy doesn't free them. */
    msg_queue_t *q = msg_queue_create(4);
    msg_queue_push(q, "a");
    msg_queue_push(q, "b");
    msg_queue_destroy(q);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fifo_order);
    RUN_TEST(test_push_copies_payload);
    RUN_TEST(test_full_queue_drops_oldest);
    RUN_TEST(test_wraps_around_many_times);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_items_still_delivered_after_shutdown);
    RUN_TEST(test_shutdown_wakes_blocked_consumer);
    RUN_TEST(test_push_wakes_blocked_consumer);
    RUN_TEST(test_destroy_frees_pending);
    return UNITY_END();
}
