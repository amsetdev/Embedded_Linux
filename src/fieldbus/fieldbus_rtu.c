/**
 * @file fieldbus_rtu.c
 * @brief Modbus RTU fieldbus driver implementation.
 *
 * Wraps the existing modbus.c RS-485/UART functions behind the
 * fieldbus_driver_t vtable. The underlying modbus.c remains
 * unchanged — this is a thin adapter.
 */

#include "fieldbus.h"
#include "modbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* Driver context                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal context for the Modbus RTU driver.
 */
typedef struct
{
    int slave_id;
} rtu_ctx_t;

/* -------------------------------------------------------------------------- */
/* Driver implementation                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the Modbus RTU driver.
 *
 * Opens the RS-485 GPIO, sets receive mode, and opens the UART port.
 *
 * @param config Fieldbus configuration (uses serial_port, baud, slave_id).
 * @return Opaque context handle, or NULL on failure.
 */
static void *rtu_init(const fieldbus_config_t *config)
{
    if (rs485_gpio_init() < 0)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] RS485 GPIO init failed\n");
    }

    rs485_rx();

    if (uart_open(config->serial_port, config->baud) < 0)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] Cannot open %s @ %d\n",
                config->serial_port,
                config->baud);
        rs485_gpio_close();
        return NULL;
    }

    printf("[FIELDBUS_RTU] Opened %s @ %d baud (slave %d)\n",
           config->serial_port,
           config->baud,
           config->slave_id);

    rtu_ctx_t *ctx = malloc(sizeof(rtu_ctx_t));

    if (!ctx)
    {
        uart_close();
        rs485_gpio_close();
        return NULL;
    }

    ctx->slave_id = config->slave_id;

    return ctx;
}

/**
 * @brief Read a single register via Modbus RTU.
 *
 * Maps the fieldbus register type to the appropriate Modbus
 * function code and executes a single-register transaction.
 *
 * @param ctx      Opaque context from rtu_init().
 * @param reg_type Register type.
 * @param addr     Register address.
 * @param value    Output: register value.
 * @return FIELDBUS_OK on success, FIELDBUS_ERR_IO on failure.
 */
static fieldbus_status_t rtu_read_register(void *ctx,
                                            fb_reg_type_t reg_type,
                                            uint16_t addr,
                                            uint16_t *value)
{
    rtu_ctx_t *rctx = (rtu_ctx_t *)ctx;
    uint8_t fc;

    switch (reg_type)
    {
    case FB_REG_COIL:
        fc = 0x01;
        break;

    case FB_REG_DISCRETE:
        fc = 0x02;
        break;

    case FB_REG_INPUT:
        fc = 0x04;
        break;

    case FB_REG_HOLDING:
    default:
        fc = 0x03;
        break;
    }

    if (mb_transaction((uint8_t)rctx->slave_id,
                       fc,
                       addr,
                       value))
    {
        return FIELDBUS_OK;
    }

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Read a contiguous block of registers via Modbus RTU.
 *
 * Loops over individual register reads. The current Modbus RTU
 * implementation reads one register per transaction, so this
 * matches existing behavior.
 *
 * @param ctx      Opaque context from rtu_init().
 * @param reg_type Register type.
 * @param start    Starting address.
 * @param count    Number of registers to read.
 * @param values   Output: array of register values.
 * @return FIELDBUS_OK if all registers read, FIELDBUS_ERR_IO otherwise.
 */
static fieldbus_status_t rtu_read_block(void *ctx,
                                         fb_reg_type_t reg_type,
                                         uint16_t start,
                                         int count,
                                         uint16_t *values)
{
    for (int i = 0; i < count; i++)
    {
        fieldbus_status_t rc =
            rtu_read_register(ctx,
                              reg_type,
                              start + (uint16_t)i,
                              &values[i]);

        if (rc != FIELDBUS_OK)
            return rc;
    }

    return FIELDBUS_OK;
}

/**
 * @brief Close the Modbus RTU driver.
 *
 * Releases UART and GPIO resources.
 *
 * @param ctx Opaque context from rtu_init().
 */
static void rtu_close(void *ctx)
{
    rs485_rx();
    rs485_gpio_close();
    uart_close();

    free(ctx);

    printf("[FIELDBUS_RTU] Driver closed\n");
}

/* -------------------------------------------------------------------------- */
/* Driver vtable                                                              */
/* -------------------------------------------------------------------------- */

static const fieldbus_driver_t modbus_rtu_driver = {
    .name          = "modbus_rtu",
    .init          = rtu_init,
    .read_register = rtu_read_register,
    .read_block    = rtu_read_block,
    .close         = rtu_close,
};

const fieldbus_driver_t *fieldbus_get_modbus_rtu(void)
{
    return &modbus_rtu_driver;
}
