/**
 * @file data.c
 * @brief Loads Modbus register configuration from smart_rtu_config.json
 *        and performs Modbus register polling.
 */

#include "data.h"
#include "modbus.h"
#include "settings.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ModbusPoint points[MAX_POINTS];
static int point_count = 0;

/*-----------------------------------------------------------*/
/* Public API                                                */
/*-----------------------------------------------------------*/

ModbusPoint *data_get_points(void)
{
    return points;
}

int data_get_count(void)
{
    return point_count;
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

        points[point_count].reg_type = REG_HOLDING;
        points[point_count].data_type = 'w';
        points[point_count].unit[0] = '\0';
        points[point_count].valid = 0;
        points[point_count].value = 0;

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
/**
 * @brief Read one Modbus point.
 *
 * @param pt Pointer to ModbusPoint
 *
 * @return 1 Success
 * @return 0 Failure
 */
/*-----------------------------------------------------------*/

int read_point(ModbusPoint *pt)
{
    uint8_t fc;

    switch (pt->reg_type)
    {
    case REG_COIL:
        fc = 0x01;
        break;

    case REG_DISCRETE:
        fc = 0x02;
        break;

    case REG_INPUT:
        fc = 0x04;
        break;

    default:
        fc = 0x03;
        break;
    }

    for (int retry = 0; retry < MAX_RETRIES; retry++)
    {
        uint16_t value = 0;

        if (mb_transaction((uint8_t)cfg.modbus_slave,
                           fc,
                           (uint16_t)pt->address,
                           &value))
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
    extern volatile int running;

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