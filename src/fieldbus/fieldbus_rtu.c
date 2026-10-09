/**
 * @file fieldbus_rtu.c
 * @brief Modbus RTU fieldbus driver implementation using libmodbus.
 *
 * Wraps libmodbus RTU functions behind the fieldbus_driver_t vtable.
 * RS-485 DE/RE direction switching is handled entirely by the kernel:
 * the device tree enables hardware RS-485 mode on the UART (DE wired to
 * the USART's own RTS pin), so libmodbus needs no custom RTS callback —
 * modbus_rtu_set_serial_mode(MODBUS_RTU_RS485) is enough.
 */

#include "fieldbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <modbus/modbus.h>

/* -------------------------------------------------------------------------- */
/* Driver context                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal context for the Modbus RTU driver.
 */
typedef struct
{
    modbus_t *mb_ctx;
    char      port[64];
    int       baud;
    int       slave_id;
} rtu_ctx_t;

/* -------------------------------------------------------------------------- */
/* Helper: map parity string to char                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Map a parity configuration string to the libmodbus parity char.
 *
 * @param parity_str Parity string ("None", "Even", "Odd").
 * @return 'N', 'E', or 'O'.
 */
static char parity_char(const char *parity_str)
{
    if (parity_str && (parity_str[0] == 'E' || parity_str[0] == 'e'))
        return 'E';
    if (parity_str && (parity_str[0] == 'O' || parity_str[0] == 'o'))
        return 'O';

    return 'N';
}

/* -------------------------------------------------------------------------- */
/* Driver implementation                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the Modbus RTU driver.
 *
 * Creates a libmodbus RTU context, configures serial parameters,
 * connects, and enables kernel hardware RS-485 mode.
 *
 * @param config Fieldbus configuration (uses serial_port, baud, slave_id, parity, stop_bits).
 * @return Opaque context handle, or NULL on failure.
 */
static void *rtu_init(const fieldbus_config_t *config)
{
    rtu_ctx_t *ctx = calloc(1, sizeof(rtu_ctx_t));

    if (!ctx)
        return NULL;

    strncpy(ctx->port, config->serial_port, sizeof(ctx->port) - 1);
    ctx->port[sizeof(ctx->port) - 1] = '\0';

    ctx->baud     = config->baud > 0 ? config->baud : 9600;
    ctx->slave_id = config->slave_id > 0 ? config->slave_id : 1;

    char parity   = parity_char(config->parity);
    int stop_bits = config->stop_bits > 0 ? config->stop_bits : 1;

    /* Create libmodbus RTU context. */
    ctx->mb_ctx = modbus_new_rtu(ctx->port,
                                 ctx->baud,
                                 parity,
                                 8,
                                 stop_bits);

    if (!ctx->mb_ctx)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] Cannot create RTU context: %s\n",
                modbus_strerror(errno));
        free(ctx);
        return NULL;
    }

    /* Set slave ID. */
    if (modbus_set_slave(ctx->mb_ctx, ctx->slave_id) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] Invalid slave id %d: %s\n",
                ctx->slave_id,
                modbus_strerror(errno));
        modbus_free(ctx->mb_ctx);
        free(ctx);
        return NULL;
    }

    /* Response timeout: 1 second. */
    modbus_set_response_timeout(ctx->mb_ctx, 1, 0);

    /* Connect (opens serial port). */
    if (modbus_connect(ctx->mb_ctx) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] Connection to %s failed: %s\n",
                ctx->port,
                modbus_strerror(errno));
        modbus_free(ctx->mb_ctx);
        free(ctx);
        return NULL;
    }

    /*
     * Enable RS-485 mode AFTER connect — modbus_rtu_set_serial_mode()
     * does a TIOCSRS485 ioctl that requires an open fd. The device
     * tree already enables RS-485 at boot (linux,rs485-enabled-at-
     * boot-time) with DE wired to the USART's hardware RTS pin, so
     * this just confirms the kernel driver has it enabled — no GPIO
     * or custom RTS callback needed.
     */
    if (modbus_rtu_set_serial_mode(ctx->mb_ctx,
                                    MODBUS_RTU_RS485) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] modbus_rtu_set_serial_mode failed: %s\n",
                modbus_strerror(errno));
    }

    /* Enable libmodbus debug output to see raw frames. */
    modbus_set_debug(ctx->mb_ctx, TRUE);

    printf("[FIELDBUS_RTU] Opened %s @ %d baud, parity=%c, "
           "stop=%d (slave %d)\n",
           ctx->port,
           ctx->baud,
           parity,
           stop_bits,
           ctx->slave_id);

    return ctx;
}

/**
 * @brief Read a single register via Modbus RTU.
 *
 * @param vctx     Opaque context from rtu_init().
 * @param reg_type Register type.
 * @param addr     Register address.
 * @param value    Output: register value.
 * @return FIELDBUS_OK on success, FIELDBUS_ERR_IO on failure.
 */
static fieldbus_status_t rtu_read_register(void *vctx,
                                            fb_reg_type_t reg_type,
                                            uint16_t addr,
                                            uint16_t *value)
{
    rtu_ctx_t *ctx = (rtu_ctx_t *)vctx;
    int rc;

    switch (reg_type)
    {
    case FB_REG_COIL:
    {
        uint8_t bit = 0;
        rc = modbus_read_bits(ctx->mb_ctx, addr, 1, &bit);
        if (rc == 1)
        {
            *value = bit;
            return FIELDBUS_OK;
        }
        break;
    }

    case FB_REG_DISCRETE:
    {
        uint8_t bit = 0;
        rc = modbus_read_input_bits(ctx->mb_ctx, addr, 1, &bit);
        if (rc == 1)
        {
            *value = bit;
            return FIELDBUS_OK;
        }
        break;
    }

    case FB_REG_INPUT:
        rc = modbus_read_input_registers(ctx->mb_ctx, addr, 1, value);
        if (rc == 1)
            return FIELDBUS_OK;
        break;

    case FB_REG_HOLDING:
    default:
        rc = modbus_read_registers(ctx->mb_ctx, addr, 1, value);
        if (rc == 1)
            return FIELDBUS_OK;
        break;
    }

    fprintf(stderr,
            "[FIELDBUS_RTU] Read error (addr %d): %s\n",
            addr,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Read a contiguous block of registers via Modbus RTU.
 *
 * Uses libmodbus block read for holding and input registers.
 * Falls back to individual reads for coils and discrete inputs.
 *
 * @param vctx     Opaque context from rtu_init().
 * @param reg_type Register type.
 * @param start    Starting address.
 * @param count    Number of registers to read.
 * @param values   Output: array of register values.
 * @return FIELDBUS_OK on success, FIELDBUS_ERR_IO on failure.
 */
static fieldbus_status_t rtu_read_block(void *vctx,
                                         fb_reg_type_t reg_type,
                                         uint16_t start,
                                         int count,
                                         uint16_t *values)
{
    rtu_ctx_t *ctx = (rtu_ctx_t *)vctx;
    int rc;

    switch (reg_type)
    {
    case FB_REG_HOLDING:
        rc = modbus_read_registers(ctx->mb_ctx, start, count, values);
        if (rc == count)
            return FIELDBUS_OK;
        break;

    case FB_REG_INPUT:
        rc = modbus_read_input_registers(ctx->mb_ctx,
                                         start,
                                         count,
                                         values);
        if (rc == count)
            return FIELDBUS_OK;
        break;

    case FB_REG_COIL:
    case FB_REG_DISCRETE:
        /* Fall back to individual reads for bit types. */
        for (int i = 0; i < count; i++)
        {
            fieldbus_status_t st =
                rtu_read_register(vctx,
                                  reg_type,
                                  start + (uint16_t)i,
                                  &values[i]);

            if (st != FIELDBUS_OK)
                return st;
        }
        return FIELDBUS_OK;

    default:
        break;
    }

    fprintf(stderr,
            "[FIELDBUS_RTU] Block read error (start %d, count %d): %s\n",
            start,
            count,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Write a single register or coil via Modbus RTU.
 *
 * @param vctx     Opaque context from rtu_init().
 * @param reg_type Register type (FB_REG_COIL or FB_REG_HOLDING).
 * @param addr     Register or coil address.
 * @param value    Value to write.
 * @return FIELDBUS_OK on success, FIELDBUS_ERR_IO on failure.
 */
static fieldbus_status_t rtu_write_register(void *vctx,
                                             fb_reg_type_t reg_type,
                                             uint16_t addr,
                                             uint16_t value)
{
    rtu_ctx_t *ctx = (rtu_ctx_t *)vctx;
    int rc;

    switch (reg_type)
    {
    case FB_REG_COIL:
        rc = modbus_write_bit(ctx->mb_ctx, addr, value ? 1 : 0);
        if (rc == 1)
            return FIELDBUS_OK;
        break;

    case FB_REG_HOLDING:
        rc = modbus_write_register(ctx->mb_ctx, addr, value);
        if (rc == 1)
            return FIELDBUS_OK;
        break;

    default:
        fprintf(stderr,
                "[FIELDBUS_RTU] Cannot write to %s registers\n",
                (reg_type == FB_REG_INPUT) ? "input" : "discrete");
        return FIELDBUS_ERR_IO;
    }

    fprintf(stderr,
            "[FIELDBUS_RTU] Write error (addr %d): %s\n",
            addr,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Write a contiguous block of registers or coils via Modbus RTU.
 *
 * @param vctx     Opaque context from rtu_init().
 * @param reg_type Register type (FB_REG_COIL or FB_REG_HOLDING).
 * @param start    Starting address.
 * @param count    Number of registers/coils.
 * @param values   Array of values to write.
 * @return FIELDBUS_OK on success, FIELDBUS_ERR_IO on failure.
 */
static fieldbus_status_t rtu_write_block(void *vctx,
                                          fb_reg_type_t reg_type,
                                          uint16_t start,
                                          int count,
                                          const uint16_t *values)
{
    rtu_ctx_t *ctx = (rtu_ctx_t *)vctx;
    int rc;

    switch (reg_type)
    {
    case FB_REG_HOLDING:
        rc = modbus_write_registers(ctx->mb_ctx,
                                    start,
                                    count,
                                    values);
        if (rc == count)
            return FIELDBUS_OK;
        break;

    case FB_REG_COIL:
    {
        /* libmodbus modbus_write_bits expects uint8_t array. */
        uint8_t *bits = malloc((size_t)count);

        if (!bits)
            return FIELDBUS_ERR_IO;

        for (int i = 0; i < count; i++)
            bits[i] = values[i] ? 1 : 0;

        rc = modbus_write_bits(ctx->mb_ctx, start, count, bits);

        free(bits);

        if (rc == count)
            return FIELDBUS_OK;
        break;
    }

    default:
        fprintf(stderr,
                "[FIELDBUS_RTU] Cannot write to %s registers\n",
                (reg_type == FB_REG_INPUT) ? "input" : "discrete");
        return FIELDBUS_ERR_IO;
    }

    fprintf(stderr,
            "[FIELDBUS_RTU] Block write error (start %d, count %d): %s\n",
            start,
            count,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Set the slave ID for subsequent RTU operations.
 *
 * @param vctx     Opaque context from rtu_init().
 * @param slave_id Modbus slave address (1-247).
 * @return FIELDBUS_OK on success, FIELDBUS_ERR_IO on failure.
 */
static fieldbus_status_t rtu_set_slave(void *vctx, int slave_id)
{
    rtu_ctx_t *ctx = (rtu_ctx_t *)vctx;

    if (slave_id < 1 || slave_id > 247)
        return FIELDBUS_ERR_IO;

    if (modbus_set_slave(ctx->mb_ctx, slave_id) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] Failed to set slave %d: %s\n",
                slave_id,
                modbus_strerror(errno));
        return FIELDBUS_ERR_IO;
    }

    ctx->slave_id = slave_id;

    return FIELDBUS_OK;
}

/**
 * @brief Close the Modbus RTU driver.
 *
 * Disconnects from the serial port and frees all resources.
 *
 * @param vctx Opaque context from rtu_init().
 */
static void rtu_close(void *vctx)
{
    rtu_ctx_t *ctx = (rtu_ctx_t *)vctx;

    if (ctx)
    {
        if (ctx->mb_ctx)
        {
            modbus_close(ctx->mb_ctx);
            modbus_free(ctx->mb_ctx);
        }

        printf("[FIELDBUS_RTU] Disconnected from %s\n",
               ctx->port);

        free(ctx);
    }
}

/* -------------------------------------------------------------------------- */
/* Driver vtable                                                              */
/* -------------------------------------------------------------------------- */

static const fieldbus_driver_t modbus_rtu_driver = {
    .name           = "modbus_rtu",
    .init           = rtu_init,
    .read_register  = rtu_read_register,
    .read_block     = rtu_read_block,
    .write_register = rtu_write_register,
    .write_block    = rtu_write_block,
    .set_slave      = rtu_set_slave,
    .close          = rtu_close,
};

const fieldbus_driver_t *fieldbus_get_modbus_rtu(void)
{
    return &modbus_rtu_driver;
}
