/**
 * @file fieldbus_tcp.c
 * @brief Modbus TCP fieldbus driver implementation.
 *
 * Wraps libmodbus TCP functions behind the fieldbus_driver_t vtable,
 * allowing the polling logic to use Modbus TCP through the same
 * protocol-agnostic interface.
 */

#include "fieldbus.h"
#include "mb_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <modbus/modbus.h>

/* -------------------------------------------------------------------------- */
/* Driver context                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Internal context for the Modbus TCP driver.
 */
typedef struct
{
    modbus_t *mb_ctx;
    char      ip[64];
    int       port;
    int       slave_id;
} tcp_ctx_t;

/* -------------------------------------------------------------------------- */
/* Driver implementation                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the Modbus TCP driver.
 *
 * Creates a libmodbus TCP context, configures the response timeout
 * and slave ID, and establishes a TCP connection.
 *
 * @param config Fieldbus configuration (uses tcp_ip, tcp_port, tcp_slave_id).
 * @return Opaque context handle, or NULL on failure.
 */
static void *tcp_init(const fieldbus_config_t *config)
{
    tcp_ctx_t *ctx = malloc(sizeof(tcp_ctx_t));

    if (!ctx)
        return NULL;

    strncpy(ctx->ip, config->tcp_ip, sizeof(ctx->ip) - 1);
    ctx->ip[sizeof(ctx->ip) - 1] = '\0';

    ctx->port     = config->tcp_port > 0
                        ? config->tcp_port
                        : MODBUS_DEFAULT_PORT;

    ctx->slave_id = config->tcp_slave_id > 0
                        ? config->tcp_slave_id
                        : MODBUS_DEFAULT_SLAVE_ID;

    ctx->mb_ctx = modbus_new_tcp(ctx->ip, ctx->port);

    if (!ctx->mb_ctx)
    {
        fprintf(stderr,
                "[FIELDBUS_TCP] Cannot create context: %s\n",
                modbus_strerror(errno));
        free(ctx);
        return NULL;
    }

    modbus_set_response_timeout(ctx->mb_ctx,
                                MODBUS_RESPONSE_TIMEOUT,
                                0);

    if (modbus_set_slave(ctx->mb_ctx, ctx->slave_id) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_TCP] Invalid slave id %d: %s\n",
                ctx->slave_id,
                modbus_strerror(errno));
        modbus_free(ctx->mb_ctx);
        free(ctx);
        return NULL;
    }

    if (modbus_connect(ctx->mb_ctx) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_TCP] Connection to %s:%d failed: %s\n",
                ctx->ip,
                ctx->port,
                modbus_strerror(errno));
        modbus_free(ctx->mb_ctx);
        free(ctx);
        return NULL;
    }

    printf("[FIELDBUS_TCP] Connected to %s:%d (slave %d)\n",
           ctx->ip,
           ctx->port,
           ctx->slave_id);

    return ctx;
}

/**
 * @brief Read a single register via Modbus TCP.
 *
 * @param vctx     Opaque context from tcp_init().
 * @param reg_type Register type.
 * @param addr     Register address.
 * @param value    Output: register value.
 * @return FIELDBUS_OK on success.
 */
static fieldbus_status_t tcp_read_register(void *vctx,
                                            fb_reg_type_t reg_type,
                                            uint16_t addr,
                                            uint16_t *value)
{
    tcp_ctx_t *ctx = (tcp_ctx_t *)vctx;
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
            "[FIELDBUS_TCP] Read error (addr %d): %s\n",
            addr,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Read a contiguous block of registers via Modbus TCP.
 *
 * Uses libmodbus block read for holding and input registers.
 * Falls back to individual reads for coils and discrete inputs.
 *
 * @param vctx     Opaque context from tcp_init().
 * @param reg_type Register type.
 * @param start    Starting address.
 * @param count    Number of registers to read.
 * @param values   Output: array of register values.
 * @return FIELDBUS_OK on success.
 */
static fieldbus_status_t tcp_read_block(void *vctx,
                                         fb_reg_type_t reg_type,
                                         uint16_t start,
                                         int count,
                                         uint16_t *values)
{
    tcp_ctx_t *ctx = (tcp_ctx_t *)vctx;
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
                tcp_read_register(vctx,
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
            "[FIELDBUS_TCP] Block read error (start %d, count %d): %s\n",
            start,
            count,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Write a single register or coil via Modbus TCP.
 *
 * @param vctx     Opaque context from tcp_init().
 * @param reg_type Register type (FB_REG_COIL or FB_REG_HOLDING).
 * @param addr     Register or coil address.
 * @param value    Value to write.
 * @return FIELDBUS_OK on success.
 */
static fieldbus_status_t tcp_write_register(void *vctx,
                                             fb_reg_type_t reg_type,
                                             uint16_t addr,
                                             uint16_t value)
{
    tcp_ctx_t *ctx = (tcp_ctx_t *)vctx;
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
                "[FIELDBUS_TCP] Cannot write to %s registers\n",
                (reg_type == FB_REG_INPUT) ? "input" : "discrete");
        return FIELDBUS_ERR_IO;
    }

    fprintf(stderr,
            "[FIELDBUS_TCP] Write error (addr %d): %s\n",
            addr,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Write a contiguous block of registers or coils via Modbus TCP.
 *
 * @param vctx     Opaque context from tcp_init().
 * @param reg_type Register type (FB_REG_COIL or FB_REG_HOLDING).
 * @param start    Starting address.
 * @param count    Number of registers/coils.
 * @param values   Array of values to write.
 * @return FIELDBUS_OK on success.
 */
static fieldbus_status_t tcp_write_block(void *vctx,
                                          fb_reg_type_t reg_type,
                                          uint16_t start,
                                          int count,
                                          const uint16_t *values)
{
    tcp_ctx_t *ctx = (tcp_ctx_t *)vctx;
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
                "[FIELDBUS_TCP] Cannot write to %s registers\n",
                (reg_type == FB_REG_INPUT) ? "input" : "discrete");
        return FIELDBUS_ERR_IO;
    }

    fprintf(stderr,
            "[FIELDBUS_TCP] Block write error (start %d, count %d): %s\n",
            start,
            count,
            modbus_strerror(errno));

    return FIELDBUS_ERR_IO;
}

/**
 * @brief Set the slave ID for subsequent TCP operations.
 *
 * @param vctx     Opaque context from tcp_init().
 * @param slave_id Modbus slave address (1-247).
 * @return FIELDBUS_OK on success, FIELDBUS_ERR_IO on failure.
 */
static fieldbus_status_t tcp_set_slave(void *vctx, int slave_id)
{
    tcp_ctx_t *ctx = (tcp_ctx_t *)vctx;

    if (slave_id < 1 || slave_id > 247)
        return FIELDBUS_ERR_IO;

    if (modbus_set_slave(ctx->mb_ctx, slave_id) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_TCP] Failed to set slave %d: %s\n",
                slave_id,
                modbus_strerror(errno));
        return FIELDBUS_ERR_IO;
    }

    ctx->slave_id = slave_id;

    return FIELDBUS_OK;
}

/**
 * @brief Close the Modbus TCP driver.
 *
 * Disconnects from the slave and frees all resources.
 *
 * @param vctx Opaque context from tcp_init().
 */
static void tcp_close(void *vctx)
{
    tcp_ctx_t *ctx = (tcp_ctx_t *)vctx;

    if (ctx)
    {
        if (ctx->mb_ctx)
        {
            modbus_close(ctx->mb_ctx);
            modbus_free(ctx->mb_ctx);
        }

        printf("[FIELDBUS_TCP] Disconnected from %s:%d\n",
               ctx->ip,
               ctx->port);

        free(ctx);
    }
}

/* -------------------------------------------------------------------------- */
/* Driver vtable                                                              */
/* -------------------------------------------------------------------------- */

static const fieldbus_driver_t modbus_tcp_driver = {
    .name           = "modbus_tcp",
    .init           = tcp_init,
    .read_register  = tcp_read_register,
    .read_block     = tcp_read_block,
    .write_register = tcp_write_register,
    .write_block    = tcp_write_block,
    .set_slave      = tcp_set_slave,
    .close          = tcp_close,
};

const fieldbus_driver_t *fieldbus_get_modbus_tcp(void)
{
    return &modbus_tcp_driver;
}
