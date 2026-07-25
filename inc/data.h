/**
 * @file data.h
 * @brief Modbus register configuration and data access.
 *
 * This module loads the register list from
 * smart_rtu_config.json and provides APIs
 * for reading Modbus registers.
 */

#ifndef DATA_H
#define DATA_H

#include "modbus.h"
#include "json.h"

#ifdef __cplusplus
extern "C" {
#endif

/*-----------------------------------------------------------
 * Configuration
 *----------------------------------------------------------*/

/**
 * @brief Maximum number of Modbus registers supported.
 */
#define MAX_POINTS     2000

/**
 * @brief Maximum register label length.
 */
#define LABEL_MAX      64

/**
 * @brief Maximum engineering unit string length.
 */
#define UNIT_MAX       16

/**
 * @brief Smart RTU configuration JSON.
 */
#define CONFIG_FILE    "smart_rtu_config.json"

/*-----------------------------------------------------------
 * Modbus Register
 *----------------------------------------------------------*/

/**
 * @brief Represents one Modbus register configured
 *        in smart_rtu_config.json.
 */
typedef struct
{
    /** Register name */
    char label[LABEL_MAX];

    /** Modbus register address */
    int address;

    /** Register type */
    RegType reg_type;

    /** Data type
     *  'w' = uint16
     *  'd' = float
     *  etc.
     */
    char data_type;

    /** Engineering unit */
    char unit[UNIT_MAX];

    /** Latest value */
    int value;

    /** Valid flag */
    int valid;

} ModbusPoint;

/*-----------------------------------------------------------
 * Public API
 *----------------------------------------------------------*/

/**
 * @brief Parse the registers[] array from
 *        smart_rtu_config.json.
 *
 * Populates the internal ModbusPoint table.
 *
 * @return 1 Success
 * @return 0 Failure
 */
int parse_registers(void);

/**
 * @brief Get pointer to configured register list.
 *
 * @return Pointer to ModbusPoint array.
 */
ModbusPoint *data_get_points(void);

/**
 * @brief Get number of configured registers.
 *
 * @return Register count.
 */
int data_get_count(void);

/**
 * @brief Read one Modbus register.
 *
 * @param pt Pointer to register.
 *
 * @return 1 Success
 * @return 0 Failure
 */
int read_point(ModbusPoint *pt);

/**
 * @brief Read all configured registers.
 */
void read_all_points(void);

#ifdef __cplusplus
}
#endif

#endif /* DATA_H */