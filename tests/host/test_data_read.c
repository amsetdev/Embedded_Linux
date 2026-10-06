/* Host tests for src/fieldbus/data.c — reading the configured registers through
 * the fieldbus driver interface. The driver is a fake: a register map per
 * (slave, table, address) and a number of failures to inject. */

#include <stdatomic.h>
#include <string.h>

#include "data.h"
#include "known_issue.h"
#include "unity.h"

/** Defined in main.c in the firmware; read_all_points() stops when it is 0. */
atomic_int running = 1;

#define REGS 64

typedef struct {
    uint16_t table[5][REGS];      /* by fb_reg_type_t (holding, input, coil, discrete) */
    int      slave;               /* current slave set via set_slave() */
    int      fail_next;           /* fail this many reads */
    int      reads, block_reads, set_slave_calls;
    int      last_slave_read;
    fb_reg_type_t last_type;
} fake_bus_t;

static fake_bus_t bus;
static int ctx_token;

static void *f_init(const fieldbus_config_t *c) { (void)c; return &ctx_token; }
static void f_close(void *ctx) { (void)ctx; }
static fieldbus_status_t f_set_slave(void *ctx, int slave) { (void)ctx; bus.slave = slave; bus.set_slave_calls++; return FIELDBUS_OK; }

static fieldbus_status_t f_read_register(void *ctx, fb_reg_type_t t, uint16_t addr, uint16_t *v)
{
    (void)ctx;
    bus.reads++;
    bus.last_type = t;
    bus.last_slave_read = bus.slave;
    if (bus.fail_next > 0) { bus.fail_next--; return FIELDBUS_ERR_IO; }
    *v = bus.table[t][addr];
    return FIELDBUS_OK;
}

static fieldbus_status_t f_read_block(void *ctx, fb_reg_type_t t, uint16_t start, int count, uint16_t *v)
{
    (void)ctx;
    bus.block_reads++;
    bus.last_type = t;
    if (bus.fail_next > 0) { bus.fail_next--; return FIELDBUS_ERR_IO; }
    for (int i = 0; i < count; i++)
        v[i] = bus.table[t][start + i];
    return FIELDBUS_OK;
}

static fieldbus_status_t f_write_register(void *ctx, fb_reg_type_t t, uint16_t a, uint16_t v) { (void)ctx; bus.table[t][a] = v; return FIELDBUS_OK; }
static fieldbus_status_t f_write_block(void *ctx, fb_reg_type_t t, uint16_t a, int n, const uint16_t *v)
{
    (void)ctx;
    for (int i = 0; i < n; i++) bus.table[t][a + i] = v[i];
    return FIELDBUS_OK;
}

static const fieldbus_driver_t fake_driver = {
    .name = "fake", .init = f_init, .read_register = f_read_register, .read_block = f_read_block,
    .write_register = f_write_register, .write_block = f_write_block, .set_slave = f_set_slave, .close = f_close,
};

static ModbusPoint point(RegType rt, char dtype, int addr, int slave)
{
    ModbusPoint p;
    memset(&p, 0, sizeof(p));
    strcpy(p.label, "P");
    p.reg_type = rt;
    p.data_type = dtype;
    p.address = addr;
    p.slave_id = slave;
    return p;
}

static void put_float(fb_reg_type_t t, int addr, float f)
{
    uint32_t raw;
    memcpy(&raw, &f, sizeof(raw));
    bus.table[t][addr] = (uint16_t)(raw >> 16);
    bus.table[t][addr + 1] = (uint16_t)(raw & 0xFFFF);
}

void setUp(void)
{
    memset(&bus, 0, sizeof(bus));
    fieldbus_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    TEST_ASSERT_EQUAL_INT(1, data_init_driver(&fake_driver, &cfg));
}

void tearDown(void) { data_close_driver(); }

static void test_uint16_holding(void)
{
    bus.table[FB_REG_HOLDING][10] = 54321;
    ModbusPoint p = point(REG_HOLDING, 'w', 10, 1);
    TEST_ASSERT_EQUAL_INT(1, read_point(&p));
    TEST_ASSERT_EQUAL_INT(54321, p.value);
    TEST_ASSERT_EQUAL_INT(1, p.valid);
}

static void test_each_table_read_from_its_own_table(void)
{
    const RegType rt[] = { REG_HOLDING, REG_INPUT, REG_COIL, REG_DISCRETE };
    const fb_reg_type_t ft[] = { FB_REG_HOLDING, FB_REG_INPUT, FB_REG_COIL, FB_REG_DISCRETE };
    for (int i = 0; i < 4; i++) {
        bus.table[ft[i]][5] = (uint16_t)(100 + i);
        ModbusPoint p = point(rt[i], 'w', 5, 1);
        read_point(&p);
        TEST_ASSERT_EQUAL_INT(ft[i], bus.last_type);
        TEST_ASSERT_EQUAL_INT(100 + i, p.value);
    }
}

static void test_float32_high_word_first(void)
{
    put_float(FB_REG_INPUT, 20, -1234.5f);
    ModbusPoint p = point(REG_INPUT, 'f', 20, 1);
    TEST_ASSERT_EQUAL_INT(1, read_point(&p));
    TEST_ASSERT_EQUAL_FLOAT(-1234.5f, p.float_value);
    TEST_ASSERT_EQUAL_INT(-1234, p.value);
    TEST_ASSERT_EQUAL_INT(1, bus.block_reads);
}

static void test_int32_high_word_first_and_signed(void)
{
    bus.table[FB_REG_HOLDING][30] = 0xFFFE;   /* -123456 = 0xFFFE1DC0 */
    bus.table[FB_REG_HOLDING][31] = 0x1DC0;
    ModbusPoint p = point(REG_HOLDING, 'd', 30, 1);
    TEST_ASSERT_EQUAL_INT(1, read_point(&p));
    TEST_ASSERT_EQUAL_INT(-123456, p.value);
}

static void test_int32_above_2_pow_24_kept_exact(void)
{
    bus.table[FB_REG_HOLDING][30] = 0x0100;   /* 16777217 = 2^24 + 1 */
    bus.table[FB_REG_HOLDING][31] = 0x0001;
    ModbusPoint p = point(REG_HOLDING, 'd', 30, 1);
    read_point(&p);
    TEST_ASSERT_EQUAL_INT(16777217, p.value);   /* telemetry prints value (%d) for int32 */
}

static void test_slave_switched_per_point(void)
{
    ModbusPoint a = point(REG_HOLDING, 'w', 1, 3);
    ModbusPoint b = point(REG_HOLDING, 'w', 1, 7);
    read_point(&a);
    TEST_ASSERT_EQUAL_INT(3, bus.last_slave_read);
    read_point(&b);
    TEST_ASSERT_EQUAL_INT(7, bus.last_slave_read);
}

static void test_retries_then_succeeds(void)
{
    bus.fail_next = MAX_RETRIES - 1;
    bus.table[FB_REG_HOLDING][2] = 9;
    ModbusPoint p = point(REG_HOLDING, 'w', 2, 1);
    TEST_ASSERT_EQUAL_INT(1, read_point(&p));
    TEST_ASSERT_EQUAL_INT(MAX_RETRIES, bus.reads);
    TEST_ASSERT_EQUAL_INT(9, p.value);
}

static void test_failed_read_marked_invalid(void)
{
    bus.fail_next = 100;
    ModbusPoint p = point(REG_HOLDING, 'w', 2, 1);
    p.valid = 1;
    TEST_ASSERT_EQUAL_INT(0, read_point(&p));
    TEST_ASSERT_EQUAL_INT(0, p.valid);
    TEST_ASSERT_EQUAL_INT(MAX_RETRIES, bus.reads);

    ModbusPoint f = point(REG_HOLDING, 'f', 2, 1);
    f.valid = 1;
    TEST_ASSERT_EQUAL_INT(0, read_point(&f));
    TEST_ASSERT_EQUAL_INT(0, f.valid);
}

static void test_no_driver_marks_invalid(void)
{
    data_close_driver();
    ModbusPoint p = point(REG_HOLDING, 'w', 2, 1);
    p.valid = 1;
    TEST_ASSERT_EQUAL_INT(0, read_point(&p));
    TEST_ASSERT_EQUAL_INT(0, p.valid);
}

static void test_write_register_and_block(void)
{
    TEST_ASSERT_EQUAL_INT(1, data_write_register(1, REG_HOLDING, 40, 77));
    TEST_ASSERT_EQUAL_INT(77, bus.table[FB_REG_HOLDING][40]);
    uint16_t v[2] = { 0x1234, 0x5678 };
    TEST_ASSERT_EQUAL_INT(1, data_write_block(1, REG_HOLDING, 41, 2, v));
    TEST_ASSERT_EQUAL_HEX16(0x5678, bus.table[FB_REG_HOLDING][42]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_uint16_holding);
    RUN_TEST(test_each_table_read_from_its_own_table);
    RUN_TEST(test_float32_high_word_first);
    RUN_TEST(test_int32_high_word_first_and_signed);
    RUN_TEST(test_int32_above_2_pow_24_kept_exact);
    RUN_TEST(test_slave_switched_per_point);
    RUN_TEST(test_retries_then_succeeds);
    RUN_TEST(test_failed_read_marked_invalid);
    RUN_TEST(test_no_driver_marks_invalid);
    RUN_TEST(test_write_register_and_block);
    return UNITY_END();
}
