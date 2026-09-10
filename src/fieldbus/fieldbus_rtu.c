/**
 * @file fieldbus_rtu.c
 * @brief Modbus RTU fieldbus driver implementation using libmodbus.
 *
 * Wraps libmodbus RTU functions behind the fieldbus_driver_t vtable.
 * Uses modbus_rtu_set_custom_rts() with a GPIO callback to toggle
 * the RS-485 DE pin (PE10 on gpiochip4 line 10).
 */

#include "fieldbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

#include <modbus/modbus.h>
#include <linux/serial.h>

/* -------------------------------------------------------------------------- */
/* GPIO DE pin configuration                                                  */
/* -------------------------------------------------------------------------- */

#define RS485_GPIOCHIP  "/dev/gpiochip4"
#define RS485_GPIO_LINE 10

/** @brief Pre-TX delay in microseconds (libmodbus RTS delay). */
#define RS485_RTS_DELAY_US 200

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
    int       gpio_fd;
    int       gpio_line_fd;
} rtu_ctx_t;

/** @brief Static pointer for RTS callback (single RTU instance). */
static rtu_ctx_t *g_rtu_ctx = NULL;

/* -------------------------------------------------------------------------- */
/* GPIO DE control                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initialize the RS-485 DE GPIO pin.
 *
 * Opens gpiochip4 and requests line 10 as output, initially LOW (RX mode).
 *
 * @param ctx RTU driver context.
 * @return 0 on success, -1 on failure.
 */
static int gpio_de_init(rtu_ctx_t *ctx)
{
    ctx->gpio_fd = open(RS485_GPIOCHIP, O_RDONLY);

    if (ctx->gpio_fd < 0)
    {
        perror("[FIELDBUS_RTU] open /dev/gpiochip4");
        return -1;
    }

    struct gpiohandle_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0]    = RS485_GPIO_LINE;
    req.lines             = 1;
    req.flags             = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = 0;
    strncpy(req.consumer_label,
            "modbus_rtu_de",
            sizeof(req.consumer_label) - 1);

    if (ioctl(ctx->gpio_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0)
    {
        perror("[FIELDBUS_RTU] GPIO_GET_LINEHANDLE_IOCTL");
        close(ctx->gpio_fd);
        ctx->gpio_fd = -1;
        return -1;
    }

    ctx->gpio_line_fd = req.fd;

    printf("[FIELDBUS_RTU] GPIO DE (PE10) init OK\n");

    return 0;
}

/**
 * @brief Set the DE GPIO pin value.
 *
 * @param ctx RTU driver context.
 * @param on  1 for HIGH (TX mode), 0 for LOW (RX mode).
 */
static void gpio_de_set(rtu_ctx_t *ctx, int on)
{
    if (ctx->gpio_line_fd < 0)
        return;

    struct gpiohandle_data d;
    memset(&d, 0, sizeof(d));
    d.values[0] = on ? 1 : 0;

    ioctl(ctx->gpio_line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d);
}

/**
 * @brief Release GPIO DE resources.
 *
 * @param ctx RTU driver context.
 */
static void gpio_de_close(rtu_ctx_t *ctx)
{
    if (ctx->gpio_line_fd >= 0)
    {
        close(ctx->gpio_line_fd);
        ctx->gpio_line_fd = -1;
    }

    if (ctx->gpio_fd >= 0)
    {
        close(ctx->gpio_fd);
        ctx->gpio_fd = -1;
    }
}

/**
 * @brief Custom RTS callback for libmodbus.
 *
 * Called by libmodbus before TX (on=1) and after TX (on=0) to
 * control the RS-485 DE pin direction.
 *
 * @param mb_ctx libmodbus context (unused — context via static pointer).
 * @param on     1 = transmit mode, 0 = receive mode.
 */
static void rtu_rts_callback(modbus_t *mb_ctx, int on)
{
    (void)mb_ctx;

    printf("[FIELDBUS_RTU] RTS callback: DE=%s\n",
           on ? "HIGH (TX)" : "LOW (RX)");

    if (g_rtu_ctx)
        gpio_de_set(g_rtu_ctx, on);
}

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
 * sets up the custom RTS callback for GPIO DE control, and connects.
 *
 * @param config Fieldbus configuration (uses serial_port, baud, slave_id, parity, stop_bits).
 * @return Opaque context handle, or NULL on failure.
 */
static void *rtu_init(const fieldbus_config_t *config)
{
    rtu_ctx_t *ctx = calloc(1, sizeof(rtu_ctx_t));

    if (!ctx)
        return NULL;

    ctx->gpio_fd      = -1;
    ctx->gpio_line_fd = -1;

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

    /* Initialize GPIO DE pin. */
    if (gpio_de_init(ctx) < 0)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] GPIO DE init failed — "
                "continuing without DE control\n");
    }

    /*
     * Store context for the static RTS callback.
     * Set custom RTS and delay BEFORE connect — these just store
     * values in the libmodbus context struct and don't need an
     * open fd.
     */
    g_rtu_ctx = ctx;

    modbus_rtu_set_custom_rts(ctx->mb_ctx, rtu_rts_callback);
    modbus_rtu_set_rts_delay(ctx->mb_ctx, RS485_RTS_DELAY_US);

    /* Connect (opens serial port). */
    if (modbus_connect(ctx->mb_ctx) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] Connection to %s failed: %s\n",
                ctx->port,
                modbus_strerror(errno));
        g_rtu_ctx = NULL;
        gpio_de_close(ctx);
        modbus_free(ctx->mb_ctx);
        free(ctx);
        return NULL;
    }

    /*
     * Set RS-485 mode AFTER connect — modbus_rtu_set_serial_mode()
     * does a TIOCSRS485 ioctl that requires an open fd.
     */
    if (modbus_rtu_set_serial_mode(ctx->mb_ctx,
                                    MODBUS_RTU_RS485) == -1)
    {
        fprintf(stderr,
                "[FIELDBUS_RTU] modbus_rtu_set_serial_mode failed: %s "
                "— continuing with custom RTS only\n",
                modbus_strerror(errno));
    }

    /*
     * Disable kernel RS485 on the raw fd.
     *
     * On the STM32MP157F-DK2 the RS485 DE pin (PE10) is routed to
     * a GPIO, NOT to the USART's hardware RTS/DE pin. The kernel
     * RS485 mode would toggle the wrong pin (or none at all). Our
     * custom RTS callback handles PE10 via GPIO instead.
     *
     * libmodbus still sees serial_mode == RS485 internally, so it
     * will call the custom RTS callback before/after every TX.
     */
    {
        int fd = modbus_get_socket(ctx->mb_ctx);
        struct serial_rs485 rs485conf;
        memset(&rs485conf, 0, sizeof(rs485conf));
        rs485conf.flags = 0;
        if (ioctl(fd, TIOCSRS485, &rs485conf) == 0)
        {
            printf("[FIELDBUS_RTU] Kernel RS485 disabled — "
                   "using GPIO DE (PE10)\n");
        }
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

        gpio_de_close(ctx);

        if (g_rtu_ctx == ctx)
            g_rtu_ctx = NULL;

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
