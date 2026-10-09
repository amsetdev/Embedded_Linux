/**
 * @file data.c
 * @brief Loads Modbus register configuration from smart_rtu_config.json
 *        and performs Modbus register polling via the fieldbus
 *        abstraction layer.
 */

#include "data.h"
#include "settings.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

static ModbusPoint points[MAX_POINTS];
static int point_count = 0;

/* -------------------------------------------------------------------------- */
/* Fieldbus driver state                                                      */
/* -------------------------------------------------------------------------- */

static const fieldbus_driver_t *rtu_driver = NULL;
static void *rtu_ctx = NULL;

/*-----------------------------------------------------------*/
/* Fieldbus Driver Management                                */
/*-----------------------------------------------------------*/

/**
 * @brief Initialize the fieldbus driver for register reading.
 *
 * @param drv    Pointer to the fieldbus driver vtable.
 * @param config Pointer to the driver configuration.
 * @return 1 on success, 0 on failure.
 */
int data_init_driver(const fieldbus_driver_t *drv,
                     const fieldbus_config_t *config)
{
    if (!drv || !config)
        return 0;

    rtu_driver = drv;
    rtu_ctx = drv->init(config);

    if (!rtu_ctx)
    {
        fprintf(stderr,
                "[DATA] Failed to initialize %s driver\n",
                drv->name);
        rtu_driver = NULL;
        return 0;
    }

    printf("[DATA] Fieldbus driver initialized: %s\n",
           drv->name);

    return 1;
}

/**
 * @brief Close the fieldbus driver.
 */
void data_close_driver(void)
{
    if (rtu_driver && rtu_ctx)
    {
        rtu_driver->close(rtu_ctx);
        rtu_ctx = NULL;
        rtu_driver = NULL;
        printf("[DATA] Fieldbus driver closed\n");
    }
}

/*-----------------------------------------------------------*/
/* Public API                                                */
/*-----------------------------------------------------------*/

/**
 * @brief Get pointer to the configured register list.
 *
 * @return Pointer to the internal ModbusPoint array.
 */
ModbusPoint *data_get_points(void)
{
    return points;
}

/**
 * @brief Get the number of configured registers.
 *
 * @return Number of registers parsed from configuration.
 */
int data_get_count(void)
{
    return point_count;
}

/*-----------------------------------------------------------*/
/* Register type / data type string mapping                  */
/*-----------------------------------------------------------*/

/**
 * @brief Map a JSON register type string to RegType enum.
 *
 * @param s Type string from config ("holding", "input", "coil", "discrete").
 * @return Corresponding RegType value; defaults to REG_HOLDING.
 */
static RegType reg_type_from_string(const char *s)
{
    if (strcmp(s, "input") == 0)
        return REG_INPUT;
    if (strcmp(s, "coil") == 0)
        return REG_COIL;
    if (strcmp(s, "discrete") == 0)
        return REG_DISCRETE;

    return REG_HOLDING;
}

/**
 * @brief Map a JSON data type string to a type character.
 *
 * @param s Data type string from config ("uint16", "word", "float", "float32", "bool").
 * @return 'w' for uint16/word, 'f' for float/float32, 'b' for bool.
 */
static char data_type_from_string(const char *s)
{
    if (strcmp(s, "float") == 0 || strcmp(s, "float32") == 0)
        return 'f';
    if (strcmp(s, "int32") == 0)
        return 'd';
    if (strcmp(s, "bool") == 0)
        return 'b';

    return 'w';
}

/*-----------------------------------------------------------*/
/**
 * @brief Parse registers[] array from smart_rtu_config.json
 *
 * Example:
 * "registers":
 * [
 *   {
 *      "label":"REG1",
 *      "address":0
 *   },
 *   {
 *      "label":"REG2",
 *      "address":1
 *   }
 * ]
 *
 * @return 1 Success
 * @return 0 Failure
 */
/*-----------------------------------------------------------*/

int parse_registers(void)
{
    char *json = read_file(SETTINGS_FILE);

    if (json == NULL)
    {
        return -1;
    }

    char *p = strstr(json, "\"registers\"");

    if (p == NULL)
    {
        free(json);
        return 0;
    }

    p = strchr(p, '[');

    if (p == NULL)
    {
        free(json);
        return 0;
    }

    p++;

    point_count = 0;

    while (*p && *p != ']')
    {
        if (*p != '{')
        {
            p++;
            continue;
        }

        if (point_count >= MAX_POINTS)
            break;

        json_get_string_from(
            p,
            "label",
            points[point_count].label,
            sizeof(points[point_count].label));

        json_get_int_from(
            p,
            "address",
            &points[point_count].address);

        char type_str[32] = "holding";
        char dtype_str[32] = "uint16";

        json_get_string_from(p, "type", type_str, sizeof(type_str));
        json_get_string_from(p, "data_type", dtype_str, sizeof(dtype_str));

        points[point_count].reg_type = reg_type_from_string(type_str);
        points[point_count].data_type = data_type_from_string(dtype_str);
        points[point_count].float_value = 0.0f;
        points[point_count].unit[0] = '\0';
        points[point_count].valid = 0;
        points[point_count].value = 0;

        /* Per-register slave_id; default to global modbus_slave. */
        int sid = 0;
        if (json_get_int_from(p, "slave_id", &sid) == 0 &&
            sid >= 1 && sid <= 247)
        {
            points[point_count].slave_id = sid;
        }
        else
        {
            points[point_count].slave_id = settings_get_slave();
        }

        point_count++;

        p = strchr(p, '}');

        if (p == NULL)
            break;

        p++;
    }

    free(json);

    printf("[DATA] Loaded %d registers\n", point_count);

    return 1;
}

/*-----------------------------------------------------------*/
/* Register type mapping helper                              */
/*-----------------------------------------------------------*/

/**
 * @brief Map ModbusPoint register type to fieldbus register type.
 *
 * @param reg_type ModbusPoint register type.
 * @return Corresponding fb_reg_type_t value.
 */
static fb_reg_type_t map_reg_type(RegType reg_type)
{
    switch (reg_type)
    {
    case REG_COIL:     return FB_REG_COIL;
    case REG_DISCRETE: return FB_REG_DISCRETE;
    case REG_INPUT:    return FB_REG_INPUT;
    case REG_HOLDING:
    default:           return FB_REG_HOLDING;
    }
}

/*-----------------------------------------------------------*/
/**
 * @brief Read a 32-bit float from two consecutive Modbus registers.
 *
 * Reads 2 registers starting at pt->address via read_block(),
 * combines them into a float (big-endian word order: high word
 * first), and stores the result in pt->float_value.
 *
 * @param pt Pointer to ModbusPoint with data_type == 'f'.
 * @return 1 on success, 0 on failure.
 */
/*-----------------------------------------------------------*/

static int read_point_32bit(ModbusPoint *pt)
{
    fb_reg_type_t rt = map_reg_type(pt->reg_type);

    for (int retry = 0; retry < MAX_RETRIES; retry++)
    {
        uint16_t regs[2] = {0, 0};

        if (rtu_driver->read_block(rtu_ctx,
                                   rt,
                                   (uint16_t)pt->address,
                                   2,
                                   regs) == FIELDBUS_OK)
        {
            /* Big-endian word order: regs[0] = high, regs[1] = low */
            uint32_t raw = ((uint32_t)regs[0] << 16) | regs[1];

            if (pt->data_type == 'f')
            {
                float fval;
                memcpy(&fval, &raw, sizeof(fval));
                pt->float_value = fval;
                pt->value = (int)fval;
            }
            else
            {
                /* int32 */
                pt->value = (int32_t)raw;
                pt->float_value = (float)pt->value;
            }

            pt->valid = 1;
            return 1;
        }

        printf("[DATA] Retry %d : %s (%d) [%s]\n",
               retry + 1,
               pt->label,
               pt->address,
               pt->data_type == 'f' ? "float" : "int32");

        usleep(50000);
    }

    pt->valid = 0;
    return 0;
}

/*-----------------------------------------------------------*/
/**
 * @brief Read one Modbus point via the fieldbus driver.
 *
 * Dispatches to read_point_float() for 32-bit float registers,
 * or performs a single-register read for uint16/bool types.
 *
 * @param pt Pointer to ModbusPoint
 *
 * @return 1 Success
 * @return 0 Failure
 */
/*-----------------------------------------------------------*/

int read_point(ModbusPoint *pt)
{
    if (!rtu_driver || !rtu_ctx)
    {
        pt->valid = 0;
        return 0;
    }

    /* Switch to the register's slave ID before reading. */
    if (rtu_driver->set_slave &&
        pt->slave_id > 0)
    {
        rtu_driver->set_slave(rtu_ctx, pt->slave_id);
    }

    /* 32-bit types (float32, int32) require a 2-register block read. */
    if (pt->data_type == 'f' || pt->data_type == 'd')
        return read_point_32bit(pt);

    fb_reg_type_t rt = map_reg_type(pt->reg_type);

    for (int retry = 0; retry < MAX_RETRIES; retry++)
    {
        uint16_t value = 0;

        if (rtu_driver->read_register(rtu_ctx,
                                      rt,
                                      (uint16_t)pt->address,
                                      &value) == FIELDBUS_OK)
        {
            pt->value = value;
            pt->valid = 1;
            return 1;
        }

        printf("[DATA] Retry %d : %s (%d)\n",
               retry + 1,
               pt->label,
               pt->address);

        usleep(50000);
    }

    pt->valid = 0;

    return 0;
}

/*-----------------------------------------------------------*/
/**
 * @brief Read every configured Modbus register.
 */
/*-----------------------------------------------------------*/

void read_all_points(void)
{
    extern atomic_int running;

    int success = 0;
    int failed = 0;

    time_t start = time(NULL);

    for (int i = 0; i < point_count && running; i++)
    {
        if (read_point(&points[i]))
            success++;
        else
            failed++;

        usleep(POINT_DELAY_US);
    }

    printf("[DATA] READ DONE : OK=%d FAIL=%d TIME=%lds\n",
           success,
           failed,
           (long)(time(NULL) - start));
}

/*-----------------------------------------------------------*/
/**
 * @brief Write a single register or coil via the fieldbus driver.
 *
 * @param slave_id Modbus slave address (1-247).
 * @param reg_type Register type (REG_HOLDING or REG_COIL).
 * @param addr     Register or coil address.
 * @param value    Value to write.
 *
 * @return 1 on success, 0 on failure.
 */
/*-----------------------------------------------------------*/

int data_write_register(int slave_id,
                        RegType reg_type,
                        uint16_t addr,
                        uint16_t value)
{
    if (!rtu_driver || !rtu_ctx)
    {
        fprintf(stderr, "[DATA] No driver initialized for write\n");
        return 0;
    }

    if (!rtu_driver->write_register)
    {
        fprintf(stderr, "[DATA] Driver does not support write\n");
        return 0;
    }

    if (rtu_driver->set_slave && slave_id > 0)
        rtu_driver->set_slave(rtu_ctx, slave_id);

    fb_reg_type_t rt = map_reg_type(reg_type);

    fieldbus_status_t rc =
        rtu_driver->write_register(rtu_ctx, rt, addr, value);

    if (rc == FIELDBUS_OK)
    {
        printf("[DATA] Write OK: slave=%d addr=%d value=%u\n",
               slave_id, addr, (unsigned)value);
        return 1;
    }

    fprintf(stderr, "[DATA] Write FAIL: slave=%d addr=%d value=%u\n",
            slave_id, addr, (unsigned)value);

    return 0;
}

/*-----------------------------------------------------------*/
/**
 * @brief Write a contiguous block of registers or coils.
 *
 * @param slave_id Modbus slave address (1-247).
 * @param reg_type Register type (REG_HOLDING or REG_COIL).
 * @param start    Starting address.
 * @param count    Number of registers/coils.
 * @param values   Array of values to write.
 *
 * @return 1 on success, 0 on failure.
 */
/*-----------------------------------------------------------*/

int data_write_block(int slave_id,
                     RegType reg_type,
                     uint16_t start,
                     int count,
                     const uint16_t *values)
{
    if (!rtu_driver || !rtu_ctx)
    {
        fprintf(stderr, "[DATA] No driver initialized for write\n");
        return 0;
    }

    if (!rtu_driver->write_block)
    {
        fprintf(stderr, "[DATA] Driver does not support block write\n");
        return 0;
    }

    if (rtu_driver->set_slave && slave_id > 0)
        rtu_driver->set_slave(rtu_ctx, slave_id);

    fb_reg_type_t rt = map_reg_type(reg_type);

    fieldbus_status_t rc =
        rtu_driver->write_block(rtu_ctx, rt, start, count, values);

    if (rc == FIELDBUS_OK)
    {
        printf("[DATA] Block write OK: slave=%d start=%d count=%d\n",
               slave_id, start, count);
        return 1;
    }

    fprintf(stderr, "[DATA] Block write FAIL: slave=%d start=%d count=%d\n",
            slave_id, start, count);

    return 0;
}
