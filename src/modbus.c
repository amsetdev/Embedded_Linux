/**
 * @file modbus.c
 * @brief Modbus RTU SLAVE implementation over RS-485.
 *
 * Physical layer (RS-485 DE GPIO control, UART open/close/read-with-
 * timeout, CRC16) is unchanged from the original master implementation.
 * What changed is the transaction logic: instead of initiating a
 * request and waiting for a reply, this module listens continuously
 * for requests from an external master and replies from the shared
 * register map (mb_regmap.h).
 */

#include "modbus.h"
#include "mb_regmap.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/gpio.h>


#define RS485_GPIOCHIP  "/dev/gpiochip4"
#define RS485_GPIO_LINE 10

static int gpio_fd   = -1;
static int gpio_line = -1;

int uart_fd = -1;  /* raw serial fd — definition */

int rs485_gpio_init(void)
{
    gpio_fd = open(RS485_GPIOCHIP, O_RDONLY);
    if (gpio_fd < 0) { perror("open /dev/gpiochip4"); return -1; }

    struct gpiohandle_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0]    = RS485_GPIO_LINE;
    req.lines             = 1;
    req.flags             = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = 0;  /* LOW = RX */
    strncpy(req.consumer_label, "modbus_de", sizeof(req.consumer_label) - 1);

    if (ioctl(gpio_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        perror("GPIO_GET_LINEHANDLE_IOCTL");
        close(gpio_fd); gpio_fd = -1; return -1;
    }
    gpio_line = req.fd;
    printf("[RS485] PE10 init OK — LOW=RX ready\n");
    return 0;
}

void rs485_tx(void)
{
    if (gpio_line < 0) return;
    struct gpiohandle_data d; memset(&d, 0, sizeof(d)); d.values[0] = 1;
    ioctl(gpio_line, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d);
    usleep(RS485_PRE_TX_US);
}

void rs485_rx(void)
{
    if (gpio_line < 0) return;
    struct gpiohandle_data d; memset(&d, 0, sizeof(d)); d.values[0] = 0;
    ioctl(gpio_line, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d);
}

void rs485_gpio_close(void)
{
    if (gpio_line >= 0) { close(gpio_line); gpio_line = -1; }
    if (gpio_fd   >= 0) { close(gpio_fd);   gpio_fd   = -1; }
}

int uart_open(const char *port, int baud)
{
    uart_fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (uart_fd < 0) { perror("uart_open"); return -1; }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(uart_fd, &tty) < 0) { perror("tcgetattr"); return -1; }

    speed_t spd;
    switch (baud) {
        case 1200:   spd = B1200;   break;
        case 2400:   spd = B2400;   break;
        case 4800:   spd = B4800;   break;
        case 9600:   spd = B9600;   break;
        case 19200:  spd = B19200;  break;
        case 38400:  spd = B38400;  break;
        case 57600:  spd = B57600;  break;
        case 115200: spd = B115200; break;
        default:     spd = B9600;   break;
    }
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_iflag  = IGNBRK;
    tty.c_lflag  = 0;
    tty.c_oflag  = 0;


    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(uart_fd, TCSANOW, &tty) < 0) { perror("tcsetattr"); return -1; }
    tcflush(uart_fd, TCIOFLUSH);
    return 0;
}

void uart_close(void)
{
    if (uart_fd >= 0) { close(uart_fd); uart_fd = -1; }
}

void uart_drain_tx(void)
{
    if (uart_fd < 0) return;
    tcdrain(uart_fd);
    usleep(RS485_TX_GUARD_US);
}

void uart_flush_rx(void)
{
    if (uart_fd < 0) return;
    tcflush(uart_fd, TCIFLUSH);
}

int uart_read_timeout(uint8_t *buf, int want, int timeout_ms)
{
    int got = 0;
    struct timeval deadline;
    gettimeofday(&deadline, NULL);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_usec += (timeout_ms % 1000) * 1000;
    if (deadline.tv_usec >= 1000000) {
        deadline.tv_sec++;
        deadline.tv_usec -= 1000000;
    }

    while (got < want) {
        struct timeval now, rem;
        gettimeofday(&now, NULL);
        rem.tv_sec  = deadline.tv_sec  - now.tv_sec;
        rem.tv_usec = deadline.tv_usec - now.tv_usec;
        if (rem.tv_usec < 0) { rem.tv_sec--; rem.tv_usec += 1000000; }
        if (rem.tv_sec < 0) break;

        fd_set rds;
        FD_ZERO(&rds);
        FD_SET(uart_fd, &rds);
        int r = select(uart_fd + 1, &rds, NULL, NULL, &rem);
        if (r <= 0) break;

        int n = read(uart_fd, buf + got, want - got);
        if (n > 0) got += n;
        else if (n < 0 && errno != EAGAIN) break;
    }
    return got;
}

uint16_t mb_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

/* -------------------------------------------------------------------------- */
/* Modbus RTU SLAVE engine                                                    */
/* -------------------------------------------------------------------------- */

/** @brief Modbus exception codes (returned as an exception reply). */
enum {
    MB_EXC_ILLEGAL_FUNCTION  = 0x01,
    MB_EXC_ILLEGAL_ADDRESS   = 0x02,
    MB_EXC_ILLEGAL_VALUE     = 0x03
};

/**
 * @brief Sends a Modbus RTU reply frame (appends CRC, toggles RS485
 *        direction around the transmission).
 */
static void mb_slave_send(const uint8_t *frame, int len)
{
    uint8_t out[MB_RTU_MAX_ADU];
    if (len + 2 > (int)sizeof(out)) return;

    memcpy(out, frame, len);
    uint16_t crc = mb_crc16(out, len);
    out[len]     = crc & 0xFF;
    out[len + 1] = (crc >> 8) & 0xFF;

    rs485_tx();
    write(uart_fd, out, len + 2);
    uart_drain_tx();
    rs485_rx();
}

/**
 * @brief Builds and sends a Modbus exception reply (function code
 *        with the high bit set, plus exception code).
 */
static void mb_slave_send_exception(uint8_t slave_id, uint8_t fc, uint8_t exc)
{
    uint8_t frame[3] = { slave_id, (uint8_t)(fc | 0x80), exc };
    mb_slave_send(frame, 3);
}

/**
 * @brief Handles FC01 (read coils) / FC02 (read discrete inputs).
 */
static void handle_read_bits(uint8_t slave_id, uint8_t fc,
                              uint16_t start, uint16_t qty)
{
    if (qty == 0 || qty > 2000) {
        mb_slave_send_exception(slave_id, fc, MB_EXC_ILLEGAL_VALUE);
        return;
    }
    int max = (fc == 0x01) ? MB_NUM_COILS : MB_NUM_DISCRETE_INPUTS;
    if ((int)start + (int)qty > max) {
        mb_slave_send_exception(slave_id, fc, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    int byte_count = (qty + 7) / 8;
    uint8_t frame[3 + 250];
    frame[0] = slave_id;
    frame[1] = fc;
    frame[2] = (uint8_t)byte_count;
    memset(&frame[3], 0, byte_count);

    mb_regmap_lock();
    for (int i = 0; i < qty; i++) {
        uint8_t bit = (fc == 0x01) ? mb_regmap_get_coil(start + i)
                                    : mb_regmap_get_discrete(start + i);
        if (bit) frame[3 + i / 8] |= (uint8_t)(1u << (i % 8));
    }
    mb_regmap_unlock();

    mb_slave_send(frame, 3 + byte_count);
}

/**
 * @brief Handles FC03 (read holding regs) / FC04 (read input regs).
 */
static void handle_read_regs(uint8_t slave_id, uint8_t fc,
                              uint16_t start, uint16_t qty)
{
    if (qty == 0 || qty > 125) {
        mb_slave_send_exception(slave_id, fc, MB_EXC_ILLEGAL_VALUE);
        return;
    }
    int max = (fc == 0x03) ? MB_NUM_HOLDING_REGS : MB_NUM_INPUT_REGS;
    if ((int)start + (int)qty > max) {
        mb_slave_send_exception(slave_id, fc, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    uint8_t frame[3 + 250];
    frame[0] = slave_id;
    frame[1] = fc;
    frame[2] = (uint8_t)(qty * 2);

    mb_regmap_lock();
    for (int i = 0; i < qty; i++) {
        uint16_t v = (fc == 0x03) ? mb_regmap_get_holding(start + i)
                                   : mb_regmap_get_input(start + i);
        frame[3 + i * 2]     = (uint8_t)(v >> 8);
        frame[3 + i * 2 + 1] = (uint8_t)(v & 0xFF);
    }
    mb_regmap_unlock();

    mb_slave_send(frame, 3 + qty * 2);
}

/**
 * @brief Handles FC05 (write single coil).
 */
static void handle_write_single_coil(uint8_t slave_id,
                                      uint16_t addr, uint16_t value)
{
    if (addr >= MB_NUM_COILS) {
        mb_slave_send_exception(slave_id, 0x05, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }
    if (value != 0x0000 && value != 0xFF00) {
        mb_slave_send_exception(slave_id, 0x05, MB_EXC_ILLEGAL_VALUE);
        return;
    }

    mb_regmap_lock();
    mb_regmap_set_coil(addr, value == 0xFF00 ? 1 : 0);
    mb_regmap_unlock();

    /* Echo request back verbatim, per spec. */
    uint8_t frame[5] = {
        slave_id, 0x05,
        (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF)
    };
    uint8_t out[6] = {
        slave_id, 0x05,
        (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
        (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)
    };
    (void)frame;
    mb_slave_send(out, 6);
}

/**
 * @brief Handles FC06 (write single holding register).
 */
static void handle_write_single_reg(uint8_t slave_id,
                                     uint16_t addr, uint16_t value)
{
    if (addr >= MB_NUM_HOLDING_REGS) {
        mb_slave_send_exception(slave_id, 0x06, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    mb_regmap_lock();
    mb_regmap_set_holding(addr, value);
    mb_regmap_unlock();

    uint8_t out[6] = {
        slave_id, 0x06,
        (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
        (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)
    };
    mb_slave_send(out, 6);
}

/**
 * @brief Handles FC15 (write multiple coils).
 */
static void handle_write_multi_coils(uint8_t slave_id, const uint8_t *req, int reqlen)
{
    if (reqlen < 7) { mb_slave_send_exception(slave_id, 0x0F, MB_EXC_ILLEGAL_VALUE); return; }

    uint16_t addr = ((uint16_t)req[2] << 8) | req[3];
    uint16_t qty  = ((uint16_t)req[4] << 8) | req[5];
    uint8_t  byte_count = req[6];

    if (qty == 0 || qty > 1968 || byte_count != (qty + 7) / 8 ||
        reqlen < 7 + byte_count) {
        mb_slave_send_exception(slave_id, 0x0F, MB_EXC_ILLEGAL_VALUE);
        return;
    }
    if ((int)addr + (int)qty > MB_NUM_COILS) {
        mb_slave_send_exception(slave_id, 0x0F, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    mb_regmap_lock();
    for (int i = 0; i < qty; i++) {
        uint8_t bit = (req[7 + i / 8] >> (i % 8)) & 0x01;
        mb_regmap_set_coil(addr + i, bit);
    }
    mb_regmap_unlock();

    uint8_t out[6] = {
        slave_id, 0x0F,
        (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
        (uint8_t)(qty >> 8),  (uint8_t)(qty & 0xFF)
    };
    mb_slave_send(out, 6);
}

/**
 * @brief Handles FC16 (write multiple holding registers).
 */
static void handle_write_multi_regs(uint8_t slave_id, const uint8_t *req, int reqlen)
{
    if (reqlen < 7) { mb_slave_send_exception(slave_id, 0x10, MB_EXC_ILLEGAL_VALUE); return; }

    uint16_t addr = ((uint16_t)req[2] << 8) | req[3];
    uint16_t qty  = ((uint16_t)req[4] << 8) | req[5];
    uint8_t  byte_count = req[6];

    if (qty == 0 || qty > 123 || byte_count != qty * 2 ||
        reqlen < 7 + byte_count) {
        mb_slave_send_exception(slave_id, 0x10, MB_EXC_ILLEGAL_VALUE);
        return;
    }
    if ((int)addr + (int)qty > MB_NUM_HOLDING_REGS) {
        mb_slave_send_exception(slave_id, 0x10, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    mb_regmap_lock();
    for (int i = 0; i < qty; i++) {
        uint16_t v = ((uint16_t)req[7 + i * 2] << 8) | req[7 + i * 2 + 1];
        mb_regmap_set_holding(addr + i, v);
    }
    mb_regmap_unlock();

    uint8_t out[6] = {
        slave_id, 0x10,
        (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF),
        (uint8_t)(qty >> 8),  (uint8_t)(qty & 0xFF)
    };
    mb_slave_send(out, 6);
}

/**
 * @brief Expected total ADU length (address+fc+data+crc) for the
 *        fixed-size request function codes, once the PDU header is
 *        known. Variable-length FC15/16 are handled separately.
 */
static int mb_fixed_request_len(uint8_t fc)
{
    switch (fc) {
        case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x06:
            return 8; /* addr+fc+2addr+2data/qty+2crc */
        default:
            return 0; /* variable length (FC15/16) or unsupported */
    }
}

void mb_rtu_slave_run(uint8_t slave_id, volatile int *running)
{
    if (uart_fd < 0) return;

    uint8_t buf[MB_RTU_MAX_ADU];

    printf("[MODBUS RTU] Slave listening — id=%d\n", slave_id);

    while (*running) {
        /* Wait for the first byte of a new frame. */
        int n = uart_read_timeout(&buf[0], 1, RX_IDLE_TIMEOUT_MS);
        if (n != 1) continue; /* idle timeout, loop and re-check *running */

        /* Read function code. */
        if (uart_read_timeout(&buf[1], 1, RX_FRAME_TIMEOUT_MS) != 1) continue;

        uint8_t fc = buf[1];
        int fixed_len = mb_fixed_request_len(fc);
        int total;

        if (fixed_len > 0) {
            /* Read the remaining fixed-length bytes. */
            int remaining = fixed_len - 2;
            if (uart_read_timeout(&buf[2], remaining, RX_FRAME_TIMEOUT_MS) != remaining)
                continue; /* short/garbled frame, drop it */
            total = fixed_len;
        } else if (fc == 0x0F || fc == 0x10) {
            /* addr(2) + qty(2) + bytecount(1) = 5 more bytes first. */
            if (uart_read_timeout(&buf[2], 5, RX_FRAME_TIMEOUT_MS) != 5) continue;
            uint8_t byte_count = buf[6];
            int data_and_crc = byte_count + 2;
            if (7 + data_and_crc > MB_RTU_MAX_ADU) continue;
            if (uart_read_timeout(&buf[7], data_and_crc, RX_FRAME_TIMEOUT_MS) != data_and_crc)
                continue;
            total = 7 + data_and_crc;
        } else {
            /* Unsupported function code: drain a short window and
             * reply with an illegal-function exception if addressed
             * to us. We still need the CRC bytes to validate, but we
             * don't know the payload length — best effort: read up
             * to 2 more bytes (min frame) and validate what we can. */
            if (uart_read_timeout(&buf[2], 2, RX_FRAME_TIMEOUT_MS) != 2) continue;
            total = 4;
        }

        /* Address filtering: ignore frames not addressed to us
         * (broadcast address 0 is accepted but not replied to). */
        uint8_t addr = buf[0];
        if (addr != slave_id && addr != 0) continue;

        /* CRC check. */
        uint16_t rcrc = (uint16_t)((buf[total - 1] << 8) | buf[total - 2]);
        uint16_t ccrc = mb_crc16(buf, total - 2);
        if (rcrc != ccrc) continue; /* corrupt frame, silently drop */

        if (addr == 0) continue; /* broadcast: process locally if desired, no reply */

        switch (fc) {
            case 0x01:
            case 0x02: {
                uint16_t start = (uint16_t)((buf[2] << 8) | buf[3]);
                uint16_t qty   = (uint16_t)((buf[4] << 8) | buf[5]);
                handle_read_bits(slave_id, fc, start, qty);
                break;
            }
            case 0x03:
            case 0x04: {
                uint16_t start = (uint16_t)((buf[2] << 8) | buf[3]);
                uint16_t qty   = (uint16_t)((buf[4] << 8) | buf[5]);
                handle_read_regs(slave_id, fc, start, qty);
                break;
            }
            case 0x05: {
                uint16_t a = (uint16_t)((buf[2] << 8) | buf[3]);
                uint16_t v = (uint16_t)((buf[4] << 8) | buf[5]);
                handle_write_single_coil(slave_id, a, v);
                break;
            }
            case 0x06: {
                uint16_t a = (uint16_t)((buf[2] << 8) | buf[3]);
                uint16_t v = (uint16_t)((buf[4] << 8) | buf[5]);
                handle_write_single_reg(slave_id, a, v);
                break;
            }
            case 0x0F:
                handle_write_multi_coils(slave_id, buf, total);
                break;
            case 0x10:
                handle_write_multi_regs(slave_id, buf, total);
                break;
            default:
                mb_slave_send_exception(slave_id, fc, MB_EXC_ILLEGAL_FUNCTION);
                break;
        }
    }

    printf("[MODBUS RTU] Slave loop exiting\n");
}