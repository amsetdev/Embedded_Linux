/* Host tests for src/cloud/payload.c — the telemetry JSON, the contract with
 * the cloud: {"ts":<unix ms>,"values":{"<label>":<value>,...}}. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "payload.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static ModbusPoint pt(const char *label, char data_type, int value, float fvalue, int valid)
{
    ModbusPoint p;
    memset(&p, 0, sizeof(p));
    snprintf(p.label, sizeof(p.label), "%s", label);
    p.data_type = data_type;
    p.value = value;
    p.float_value = fvalue;
    p.valid = valid;
    return p;
}

static char buf[4096];

static void test_empty(void)
{
    int n = payload_build(buf, sizeof(buf), 1767225600123LL, NULL, 0);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1767225600123,\"values\":{}}", buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(buf), n);
}

static void test_every_data_type(void)
{
    ModbusPoint p[] = {
        pt("U16", 'w', 65535, 0, 1),
        pt("I32", 'd', -123456, 0, 1),
        pt("F32", 'f', 0, 12.345f, 1),
        pt("BIT", 'b', 1, 0, 1),
        pt("BIT0", 'b', 0, 0, 1),
    };
    payload_build(buf, sizeof(buf), 1, p, 5);
    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":1,\"values\":{\"U16\":65535,\"I32\":-123456,\"F32\":12.35,\"BIT\":true,\"BIT0\":false}}", buf);
}

static void test_failed_reads_left_out(void)
{
    ModbusPoint p[] = { pt("BAD1", 'w', 1, 0, 0), pt("OK", 'w', 2, 0, 1), pt("BAD2", 'w', 3, 0, 0) };
    payload_build(buf, sizeof(buf), 1, p, 3);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"values\":{\"OK\":2}}", buf);
}

static void test_no_comma_after_left_out_first_point(void)
{
    ModbusPoint p[] = { pt("BAD", 'w', 1, 0, 0), pt("A", 'w', 1, 0, 1), pt("B", 'w', 2, 0, 1) };
    payload_build(buf, sizeof(buf), 1, p, 3);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"values\":{\"A\":1,\"B\":2}}", buf);
}

static void test_full_buffer_stops_adding_values_and_stays_terminated(void)
{
    static ModbusPoint p[100];
    char label[16];
    for (int i = 0; i < 100; i++) {
        snprintf(label, sizeof(label), "P%03d", i);
        p[i] = pt(label, 'w', i, 0, 1);
    }
    char small[300];
    int n = payload_build(small, sizeof(small), 1, p, 100);
    TEST_ASSERT_TRUE(n < (int)sizeof(small));
    TEST_ASSERT_EQUAL_STRING("}}", small + n - 2);
    TEST_ASSERT_NOT_NULL(strstr(small, "\"P000\":0"));
    TEST_ASSERT_NULL(strstr(small, "\"P099\""));
}

static void test_nan_float_gives_valid_json(void)
{
    /* Regression: NaN/Inf were printed as nan/inf, making the whole message invalid JSON. */
    ModbusPoint p[] = { pt("N", 'f', 0, NAN, 1), pt("I", 'f', 0, -INFINITY, 1), pt("OK", 'f', 0, 1.5f, 1) };
    payload_build(buf, sizeof(buf), 1, p, 3);
    TEST_ASSERT_EQUAL_STRING("{\"ts\":1,\"values\":{\"N\":null,\"I\":null,\"OK\":1.50}}", buf);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_every_data_type);
    RUN_TEST(test_failed_reads_left_out);
    RUN_TEST(test_no_comma_after_left_out_first_point);
    RUN_TEST(test_full_buffer_stops_adding_values_and_stays_terminated);
    RUN_TEST(test_nan_float_gives_valid_json);
    return UNITY_END();
}
