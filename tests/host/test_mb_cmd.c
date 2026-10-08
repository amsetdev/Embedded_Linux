/* Host tests for src/cloud/mb_cmd.c — Modbus write commands from MQTT: the
 * message format of feat/mb_write, the safety rule (only configured, writable
 * registers are written), what goes on the bus and the response. */

#include <stdio.h>
#include <string.h>

#include "mb_cmd.h"
#include "unity.h"

/* The configuration the commands are checked against. */
static const ModbusPoint POINTS[] = {
    { "HOLD_U16",  1, 10, REG_HOLDING,  'w', "", 0, 0, 0 },
    { "HOLD_I32",  1, 20, REG_HOLDING,  'd', "", 0, 0, 0 },
    { "HOLD_F32",  1, 30, REG_HOLDING,  'f', "", 0, 0, 0 },
    { "HOLD_U16B", 1, 32, REG_HOLDING,  'w', "", 0, 0, 0 },
    { "IN_U16",    1, 40, REG_INPUT,    'w', "", 0, 0, 0 },
    { "COIL_A",    1, 60, REG_COIL,     'w', "", 0, 0, 0 },
    { "COIL_B",    1, 61, REG_COIL,     'w', "", 0, 0, 0 },
    { "DISC",      1, 70, REG_DISCRETE, 'w', "", 0, 0, 0 },
    { "S2_HOLD",   2, 10, REG_HOLDING,  'w', "", 0, 0, 0 },
};

typedef struct {
    int locks, unlocks, writes, write_fails;
    int locked_during_points, locked_during_write;
    int slave, count;
    RegType table;
    uint16_t addr, values[MB_CMD_MAX_VALUES];
} fake_t;

static fake_t fk;

static void f_lock(void *ctx) { (void)ctx; fk.locks++; }
static void f_unlock(void *ctx) { (void)ctx; fk.unlocks++; }

static const ModbusPoint *f_points(void *ctx, int *count)
{
    (void)ctx;
    fk.locked_during_points = fk.locks > fk.unlocks;
    *count = (int)(sizeof(POINTS) / sizeof(POINTS[0]));
    return POINTS;
}

static int f_write(void *ctx, int slave, RegType table, uint16_t addr, int count, const uint16_t *values)
{
    (void)ctx;
    fk.writes++;
    fk.locked_during_write = fk.locks > fk.unlocks;
    fk.slave = slave;
    fk.table = table;
    fk.addr = addr;
    fk.count = count;
    memcpy(fk.values, values, (size_t)count * sizeof(uint16_t));
    return !fk.write_fails;
}

static const mbc_ops_t ops = { NULL, f_lock, f_unlock, f_points, f_write };

static char resp[MB_CMD_RESPONSE_MAX];

void setUp(void) { memset(&fk, 0, sizeof(fk)); resp[0] = '\0'; }
void tearDown(void) { TEST_ASSERT_EQUAL_INT_MESSAGE(fk.locks, fk.unlocks, "bus lock not released"); }

static mbc_result_t cmd(const char *payload)
{
    return mb_cmd_handle(&ops, payload, strlen(payload), 0, resp, sizeof(resp));
}

static mbc_result_t single(int slave, int fc, int addr, long value)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"requestId\":\"r1\",\"method\":\"mb_write_single\","
             "\"params\":{\"slave\":%d,\"fc\":%d,\"addr\":%d,\"value\":%ld}}", slave, fc, addr, value);
    return cmd(buf);
}

static mbc_result_t multiple(int slave, int fc, int addr, int count, const char *values)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"requestId\":\"r2\",\"method\":\"mb_write_multiple\","
             "\"params\":{\"slave\":%d,\"fc\":%d,\"addr\":%d,\"count\":%d,\"values\":%s}}",
             slave, fc, addr, count, values);
    return cmd(buf);
}

static void assert_refused_with(mbc_result_t r, mbc_result_t expected, const char *detail_part)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(expected, r, resp);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fk.writes, "something was sent on the bus");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, "\"status\":\"error\""), resp);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(resp, detail_part), resp);
}

/* ---------------------------------------------------------------- accepted */

static void test_fc06_writes_configured_holding_register(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, single(1, 6, 10, 1234));
    TEST_ASSERT_EQUAL_INT(1, fk.writes);
    TEST_ASSERT_EQUAL_INT(1, fk.slave);
    TEST_ASSERT_EQUAL_INT(REG_HOLDING, fk.table);
    TEST_ASSERT_EQUAL_UINT16(10, fk.addr);
    TEST_ASSERT_EQUAL_INT(1, fk.count);
    TEST_ASSERT_EQUAL_UINT16(1234, fk.values[0]);
    TEST_ASSERT_EQUAL_STRING("{\"requestId\":\"r1\",\"method\":\"mb_write_single\",\"status\":\"ok\"}", resp);
}

static void test_fc05_writes_configured_coil(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, single(1, 5, 61, 1));
    TEST_ASSERT_EQUAL_INT(REG_COIL, fk.table);
    TEST_ASSERT_EQUAL_UINT16(61, fk.addr);
    TEST_ASSERT_EQUAL_UINT16(1, fk.values[0]);
}

static void test_fc16_writes_32bit_register_both_words(void)
{
    /* -123456 = 0xFFFE1DC0: high word first, as the gateway reads it. */
    TEST_ASSERT_EQUAL_INT(MBC_OK, multiple(1, 16, 20, 2, "[65534, 7616]"));
    TEST_ASSERT_EQUAL_INT(REG_HOLDING, fk.table);
    TEST_ASSERT_EQUAL_UINT16(20, fk.addr);
    TEST_ASSERT_EQUAL_INT(2, fk.count);
    TEST_ASSERT_EQUAL_UINT16(65534, fk.values[0]);
    TEST_ASSERT_EQUAL_UINT16(7616, fk.values[1]);
    TEST_ASSERT_EQUAL_STRING("{\"requestId\":\"r2\",\"method\":\"mb_write_multiple\",\"status\":\"ok\"}", resp);
}

static void test_fc16_spans_adjacent_configured_registers(void)
{
    /* HOLD_F32 (30, 31) + HOLD_U16B (32) */
    TEST_ASSERT_EQUAL_INT(MBC_OK, multiple(1, 16, 30, 3, "[16464, 0, 7]"));
    TEST_ASSERT_EQUAL_INT(3, fk.count);
}

static void test_fc15_writes_configured_coils(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, multiple(1, 15, 60, 2, "[1,0]"));
    TEST_ASSERT_EQUAL_INT(REG_COIL, fk.table);
    TEST_ASSERT_EQUAL_INT(2, fk.count);
    TEST_ASSERT_EQUAL_UINT16(1, fk.values[0]);
    TEST_ASSERT_EQUAL_UINT16(0, fk.values[1]);
}

static void test_slave_must_match(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, single(2, 6, 10, 5));
    TEST_ASSERT_EQUAL_INT(2, fk.slave);
    setUp();
    assert_refused_with(single(2, 6, 20, 5), MBC_REFUSED, "slave 2 addr 20 is not a configured holding register");
}

static void test_bus_locked_while_checking_and_writing(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, single(1, 6, 10, 1));
    TEST_ASSERT_EQUAL_INT(1, fk.locks);
    TEST_ASSERT_TRUE(fk.locked_during_points);
    TEST_ASSERT_TRUE(fk.locked_during_write);
}

static void test_value_limits_accepted(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, single(1, 6, 10, 0));
    TEST_ASSERT_EQUAL_INT(MBC_OK, single(1, 6, 10, 65535));
    TEST_ASSERT_EQUAL_UINT16(65535, fk.values[0]);
}

static void test_without_request_id_still_answered(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, cmd("{\"method\":\"mb_write_single\","
                                      "\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1}}"));
    TEST_ASSERT_EQUAL_STRING("{\"method\":\"mb_write_single\",\"status\":\"ok\"}", resp);
}

static void test_whitespace_and_key_order_free(void)
{
    TEST_ASSERT_EQUAL_INT(MBC_OK, cmd(" {\n \"params\" : { \"value\" : 7 , \"addr\" : 10 , \"fc\" : 6 , \"slave\" : 1 } ,\n"
                                      "   \"method\" : \"mb_write_single\" , \"requestId\" : \"x\" }\n"));
    TEST_ASSERT_EQUAL_UINT16(7, fk.values[0]);
}

/* ---------------------------------------------------------------- the safety rule */

static void test_unconfigured_address_refused(void)
{
    assert_refused_with(single(1, 6, 11, 1), MBC_REFUSED, "slave 1 addr 11 is not a configured holding register");
    assert_refused_with(single(1, 5, 62, 1), MBC_REFUSED, "not a configured coil");
}

static void test_read_only_tables_refused(void)
{
    assert_refused_with(single(1, 6, 40, 1), MBC_REFUSED, "addr 40 is IN_U16, an input register, not a holding register");
    assert_refused_with(single(1, 5, 70, 1), MBC_REFUSED, "addr 70 is DISC, a discrete input, not a coil");
}

static void test_wrong_table_for_function_code_refused(void)
{
    /* FC06 on a coil, FC05 on a holding register */
    assert_refused_with(single(1, 6, 60, 1), MBC_REFUSED, "COIL_A, a coil, not a holding register");
    assert_refused_with(single(1, 5, 10, 1), MBC_REFUSED, "HOLD_U16, a holding register, not a coil");
}

static void test_fc06_on_32bit_register_refused(void)
{
    assert_refused_with(single(1, 6, 20, 1), MBC_REFUSED, "HOLD_I32 is int32 (2 registers): write both words with fc 16");
    assert_refused_with(single(1, 6, 31, 1), MBC_REFUSED, "HOLD_F32 is float32");
}

static void test_half_of_32bit_register_refused(void)
{
    assert_refused_with(multiple(1, 16, 21, 1, "[1]"), MBC_REFUSED, "HOLD_I32 (addr 20..21) must be written as a whole");
    assert_refused_with(multiple(1, 16, 20, 1, "[1]"), MBC_REFUSED, "must be written as a whole");
    /* a range that ends in the middle of HOLD_F32 (30..31) */
    assert_refused_with(multiple(1, 16, 30, 1, "[1]"), MBC_REFUSED, "HOLD_F32 (addr 30..31) must be written as a whole");
}

static void test_range_with_one_unconfigured_word_refused(void)
{
    /* 10 is configured, 11..19 are not: nothing at all is written */
    assert_refused_with(multiple(1, 16, 10, 2, "[1,2]"), MBC_REFUSED, "addr 11 is not a configured holding register");
    assert_refused_with(multiple(1, 15, 60, 3, "[1,1,1]"), MBC_REFUSED, "addr 62 is not a configured coil");
}

static const ModbusPoint *f_no_points(void *ctx, int *count)
{
    (void)ctx;
    *count = 0;
    return POINTS;
}

static void test_empty_configuration_refuses_everything(void)
{
    static const mbc_ops_t none = { NULL, f_lock, f_unlock, f_no_points, f_write };
    const char *c = "{\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1}}";
    TEST_ASSERT_EQUAL_INT(MBC_REFUSED, mb_cmd_handle(&none, c, strlen(c), 0, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_INT(0, fk.writes);
    TEST_ASSERT_NOT_NULL(strstr(resp, "slave 1 addr 10 is not a configured holding register"));
}

/* ---------------------------------------------------------------- malformed */

static void test_malformed_commands_rejected(void)
{
    struct { const char *payload, *detail; } bad[] = {
        { "", "size" },
        { "hello", "not a JSON object" },
        { "[1]", "not a JSON object" },
        { "{\"method\":\"mb_write_single\"", "not a JSON object" },
        { "{\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1}} x", "not a JSON object" },
        { "{\"params\":{}}", "missing method" },
        { "{\"method\":\"mb_reboot\",\"params\":{}}", "unknown method" },
        { "{\"method\":\"mb_write_single\"}", "missing params" },
        { "{\"method\":\"mb_write_single\",\"params\":{\"fc\":6,\"addr\":10,\"value\":1}}", "missing params.slave" },
        { "{\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10}}", "missing params.value" },
        { "{\"method\":\"mb_write_single\",\"params\":{\"slave\":\"1\",\"fc\":6,\"addr\":10,\"value\":1}}", "not an integer: params.slave" },
        { "{\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1.5}}", "not an integer: params.value" },
        { "{\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1e3}}", "not an integer: params.value" },
        { "{\"method\":\"mb_write_multiple\",\"params\":{\"slave\":1,\"fc\":16,\"addr\":20,\"count\":2}}", "params.values must be an array" },
        { "{\"method\":\"mb_write_multiple\",\"params\":{\"slave\":1,\"fc\":16,\"addr\":20,\"count\":2,\"values\":[1,\"2\"]}}", "params.values must be an array" },
        { "{\"method\":\"mb_write_multiple\",\"params\":{\"slave\":1,\"fc\":16,\"addr\":20,\"count\":2,\"values\":[1,2,]}}", "params.values must be an array" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        setUp();
        assert_refused_with(cmd(bad[i].payload), MBC_INVALID, bad[i].detail);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, fk.locks, "bus locked for a malformed command");
    }
}

static void test_out_of_range_values_rejected(void)
{
    assert_refused_with(single(0, 6, 10, 1), MBC_INVALID, "slave must be 1..247");
    assert_refused_with(single(248, 6, 10, 1), MBC_INVALID, "slave must be 1..247");
    assert_refused_with(single(1, 6, -1, 1), MBC_INVALID, "addr out of range");
    assert_refused_with(single(1, 6, 65536, 1), MBC_INVALID, "addr out of range");
    assert_refused_with(single(1, 6, 10, 65536), MBC_INVALID, "value 65536 out of range 0..65535");
    assert_refused_with(single(1, 6, 10, -1), MBC_INVALID, "value -1 out of range 0..65535");
    assert_refused_with(single(1, 5, 60, 2), MBC_INVALID, "value 2 out of range 0..1");
    assert_refused_with(multiple(1, 15, 60, 2, "[1,255]"), MBC_INVALID, "value 255 out of range 0..1");
    assert_refused_with(single(1, 6, 10, 99999999999999999), MBC_INVALID, "value");
}

static void test_function_code_must_match_method(void)
{
    assert_refused_with(single(1, 16, 10, 1), MBC_INVALID, "mb_write_single needs fc 5 or 6");
    assert_refused_with(single(1, 3, 10, 1), MBC_INVALID, "mb_write_single needs fc 5 or 6");
    assert_refused_with(multiple(1, 6, 10, 1, "[1]"), MBC_INVALID, "mb_write_multiple needs fc 15 or 16");
}

static void test_count_and_values_must_agree(void)
{
    assert_refused_with(multiple(1, 16, 20, 2, "[1]"), MBC_INVALID, "count is 2 but values has 1");
    assert_refused_with(multiple(1, 16, 20, 0, "[]"), MBC_INVALID, "count must be 1..123");
    assert_refused_with(multiple(1, 16, 20, 124, "[1]"), MBC_INVALID, "count must be 1..123");
    assert_refused_with(multiple(1, 16, 65535, 2, "[1,2]"), MBC_INVALID, "addr out of range");
}

static void test_too_many_values_rejected(void)
{
    char values[1024] = "[";
    for (int i = 0; i < MB_CMD_MAX_VALUES + 1; i++)
        strcat(values, i ? ",1" : "1");
    strcat(values, "]");
    assert_refused_with(multiple(1, 16, 10, MB_CMD_MAX_VALUES, values), MBC_INVALID, "params.values must be an array");
}

static void test_payload_with_nul_or_too_large_rejected(void)
{
    char with_nul[] = "{\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1}}";
    with_nul[3] = '\0';
    TEST_ASSERT_EQUAL_INT(MBC_INVALID, mb_cmd_handle(&ops, with_nul, sizeof(with_nul) - 1, 0, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_INT(MBC_INVALID, mb_cmd_handle(&ops, with_nul, MB_CMD_MAX_PAYLOAD + 1, 0, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"error\",\"detail\":\"size\"}", resp);
    TEST_ASSERT_EQUAL_INT(0, fk.writes);
}

static void test_payload_not_nul_terminated(void)
{
    const char *c = "{\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":9}}";
    char buf[256];
    size_t n = strlen(c);
    memcpy(buf, c, n);
    memset(buf + n, '}', sizeof(buf) - n);
    TEST_ASSERT_EQUAL_INT(MBC_OK, mb_cmd_handle(&ops, buf, n, 0, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT16(9, fk.values[0]);
}

static void test_retained_command_never_executed(void)
{
    const char *c = "{\"requestId\":\"old\",\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1}}";
    TEST_ASSERT_EQUAL_INT(MBC_INVALID, mb_cmd_handle(&ops, c, strlen(c), 1, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_INT(0, fk.writes);
    TEST_ASSERT_EQUAL_STRING("{\"requestId\":\"old\",\"method\":\"mb_write_single\",\"status\":\"error\","
                             "\"detail\":\"retained commands are not executed\"}", resp);
}

/* ---------------------------------------------------------------- bus errors, response */

static void test_slave_not_confirming_reported(void)
{
    fk.write_fails = 1;
    TEST_ASSERT_EQUAL_INT(MBC_BUS_ERROR, single(1, 6, 10, 1));
    TEST_ASSERT_EQUAL_INT(1, fk.writes);
    TEST_ASSERT_EQUAL_STRING("{\"requestId\":\"r1\",\"method\":\"mb_write_single\",\"status\":\"error\","
                             "\"detail\":\"slave 1 did not confirm the write (timeout or exception)\"}", resp);
}

static void test_request_id_escaped_in_response(void)
{
    const char *c = "{\"requestId\":\"a\\\"b\",\"method\":\"mb_write_single\",\"params\":{\"slave\":1,\"fc\":6,\"addr\":10,\"value\":1}}";
    cmd(c);
    /* whatever json_get_string makes of the escaped quote, the response stays valid JSON */
    TEST_ASSERT_NULL_MESSAGE(strstr(resp, "\"a\"b\""), resp);
}

static void test_response_fits_with_longest_fields(void)
{
    char c[512];
    char id[200];
    memset(id, '"' + 1, sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    snprintf(c, sizeof(c), "{\"requestId\":\"%s\",\"method\":\"mb_write_multiple\","
             "\"params\":{\"slave\":1,\"fc\":16,\"addr\":21,\"count\":1,\"values\":[1]}}", id);
    cmd(c);
    TEST_ASSERT_LESS_THAN(MB_CMD_RESPONSE_MAX, strlen(resp));
    TEST_ASSERT_EQUAL_CHAR('}', resp[strlen(resp) - 1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fc06_writes_configured_holding_register);
    RUN_TEST(test_fc05_writes_configured_coil);
    RUN_TEST(test_fc16_writes_32bit_register_both_words);
    RUN_TEST(test_fc16_spans_adjacent_configured_registers);
    RUN_TEST(test_fc15_writes_configured_coils);
    RUN_TEST(test_slave_must_match);
    RUN_TEST(test_bus_locked_while_checking_and_writing);
    RUN_TEST(test_value_limits_accepted);
    RUN_TEST(test_without_request_id_still_answered);
    RUN_TEST(test_whitespace_and_key_order_free);
    RUN_TEST(test_unconfigured_address_refused);
    RUN_TEST(test_read_only_tables_refused);
    RUN_TEST(test_wrong_table_for_function_code_refused);
    RUN_TEST(test_fc06_on_32bit_register_refused);
    RUN_TEST(test_half_of_32bit_register_refused);
    RUN_TEST(test_range_with_one_unconfigured_word_refused);
    RUN_TEST(test_empty_configuration_refuses_everything);
    RUN_TEST(test_malformed_commands_rejected);
    RUN_TEST(test_out_of_range_values_rejected);
    RUN_TEST(test_function_code_must_match_method);
    RUN_TEST(test_count_and_values_must_agree);
    RUN_TEST(test_too_many_values_rejected);
    RUN_TEST(test_payload_with_nul_or_too_large_rejected);
    RUN_TEST(test_payload_not_nul_terminated);
    RUN_TEST(test_retained_command_never_executed);
    RUN_TEST(test_slave_not_confirming_reported);
    RUN_TEST(test_request_id_escaped_in_response);
    RUN_TEST(test_response_fits_with_longest_fields);
    return UNITY_END();
}
