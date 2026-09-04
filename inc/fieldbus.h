/**
 * @file fieldbus.h
 * @brief Fieldbus abstraction layer interface.
 *
 * Defines a protocol-agnostic driver interface for industrial
 * fieldbus protocols. Each protocol (Modbus RTU, Modbus TCP, etc.)
 * implements the same vtable, allowing the polling logic to be
 * completely independent of the underlying protocol.
 *
 * To add a new protocol:
 * 1. Create a new source file (e.g., fieldbus_opcua.c).
 * 2. Implement the four vtable functions.
 * 3. Provide a getter function (e.g., fieldbus_get_opcua()).
 * 4. Add the source file to the makefile.
 */

#ifndef FIELDBUS_H
#define FIELDBUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Status Codes                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Status codes returned by all driver operations.
 */
typedef enum
{
    FIELDBUS_OK       =  0,
    FIELDBUS_ERR_INIT = -1,
    FIELDBUS_ERR_IO   = -2,
    FIELDBUS_ERR_CRC  = -3,
    FIELDBUS_ERR_TMO  = -4
} fieldbus_status_t;

/* -------------------------------------------------------------------------- */
/* Register Types                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Register type enumeration (shared across all protocols).
 */
typedef enum
{
    FB_REG_HOLDING  = 0,
    FB_REG_INPUT    = 1,
    FB_REG_COIL     = 2,
    FB_REG_DISCRETE = 3
} fb_reg_type_t;

/* -------------------------------------------------------------------------- */
/* Driver Configuration                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Protocol-agnostic configuration structure.
 *
 * Each protocol uses the relevant fields and ignores the rest.
 */
typedef struct
{
    /** @brief Protocol name ("modbus_rtu" or "modbus_tcp"). */
    char protocol[32];

    /* Modbus RTU specific */

    /** @brief Serial port device path. */
    char serial_port[64];

    /** @brief Baud rate. */
    int baud;

    /** @brief Modbus RTU slave address. */
    int slave_id;

    /** @brief Parity setting ("None", "Even", "Odd"). */
    char parity[16];

    /** @brief Number of stop bits. */
    int stop_bits;

    /* Modbus TCP specific */

    /** @brief Modbus TCP slave IP address. */
    char tcp_ip[64];

    /** @brief Modbus TCP port number. */
    int tcp_port;

    /** @brief Modbus TCP slave ID (Unit Identifier). */
    int tcp_slave_id;

} fieldbus_config_t;

/* -------------------------------------------------------------------------- */
/* Driver Interface (vtable)                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fieldbus driver vtable.
 *
 * Every protocol driver implements these four functions.
 */
typedef struct fieldbus_driver
{
    /** @brief Driver name (e.g., "modbus_rtu", "modbus_tcp"). */
    const char *name;

    /**
     * @brief Initialize the driver.
     *
     * @param config Protocol configuration.
     * @return Opaque context handle, or NULL on failure.
     */
    void *(*init)(const fieldbus_config_t *config);

    /**
     * @brief Read a single register.
     *
     * @param ctx      Opaque context from init().
     * @param reg_type Register type.
     * @param addr     Register address.
     * @param value    Output: register value.
     * @return FIELDBUS_OK on success.
     */
    fieldbus_status_t (*read_register)(void *ctx,
                                       fb_reg_type_t reg_type,
                                       uint16_t addr,
                                       uint16_t *value);

    /**
     * @brief Read a contiguous block of registers.
     *
     * @param ctx      Opaque context from init().
     * @param reg_type Register type.
     * @param start    Starting address.
     * @param count    Number of registers to read.
     * @param values   Output: array of register values.
     * @return FIELDBUS_OK on success.
     */
    fieldbus_status_t (*read_block)(void *ctx,
                                    fb_reg_type_t reg_type,
                                    uint16_t start,
                                    int count,
                                    uint16_t *values);

    /**
     * @brief Close the driver and free resources.
     *
     * @param ctx Opaque context from init().
     */
    void (*close)(void *ctx);

} fieldbus_driver_t;

/* -------------------------------------------------------------------------- */
/* Driver Getters                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Get the Modbus RTU driver.
 *
 * @return Pointer to the Modbus RTU driver vtable.
 */
const fieldbus_driver_t *fieldbus_get_modbus_rtu(void);

/**
 * @brief Get the Modbus TCP driver.
 *
 * @return Pointer to the Modbus TCP driver vtable.
 */
const fieldbus_driver_t *fieldbus_get_modbus_tcp(void);

#ifdef __cplusplus
}
#endif

#endif /* FIELDBUS_H */
