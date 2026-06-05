/**
 * modbus.c — RS485 GPIO, raw UART, and Modbus RTU transaction layer
 */

#include "modbus.h"

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

/* ============================================================================
 * RS485 DE PIN — PE10 = gpiochip4 line 10
 * ========================================================================== */
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

/* ============================================================================
 * RAW UART
 * ========================================================================== */
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

    /* 8N1, raw mode */
    tty.c_cflag  = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_iflag  = IGNBRK;
    tty.c_lflag  = 0;
    tty.c_oflag  = 0;

    /* Non-blocking reads — we use select() for timeout */
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

/* ============================================================================
 * MODBUS RTU — manual frame builder
 * ========================================================================== */
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

int mb_reply_len(uint8_t fc)
{
    switch (fc) {
        case 0x01:  /* read coils          */
        case 0x02:  /* read discrete input */
            return 6;
        case 0x03:  /* read holding regs   */
        case 0x04:  /* read input regs     */
            return 7;
        default:
            return 7;
    }
}

int mb_transaction(uint8_t slave, uint8_t fc, uint16_t addr, uint16_t *value)
{
    if (uart_fd < 0) return 0;

    /* Build request frame */
    uint8_t req[8];
    req[0] = slave;
    req[1] = fc;
    req[2] = (addr >> 8) & 0xFF;
    req[3] =  addr       & 0xFF;
    req[4] = 0x00;
    req[5] = 0x01;
    uint16_t crc = mb_crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    uart_flush_rx();

    /* STEP 1: assert TX */
    rs485_tx();

    /* STEP 2: transmit */
    int wr = write(uart_fd, req, 8);
    if (wr != 8) { rs485_rx(); return 0; }

    /* STEP 3: drain HW shift register */
    uart_drain_tx();

    /* STEP 4: switch to RX */
    rs485_rx();

    /* STEP 5: read reply */
    int want = mb_reply_len(fc);
    uint8_t rsp[16];
    memset(rsp, 0, sizeof(rsp));
    int got = uart_read_timeout(rsp, want, RX_TIMEOUT_MS);

    if (got != want) return 0;

    /* STEP 6: validate CRC */
    uint16_t rcrc = (rsp[got-1] << 8) | rsp[got-2];
    uint16_t ccrc = mb_crc16(rsp, got - 2);
    if (rcrc != ccrc) return 0;

    /* STEP 7: check slave addr + FC */
    if (rsp[0] != slave) return 0;
    if (rsp[1] != fc)    return 0;

    /* STEP 8: extract value */
    switch (fc) {
        case 0x01:
        case 0x02:
            *value = rsp[3] & 0x01;
            break;
        case 0x03:
        case 0x04:
            *value = ((uint16_t)rsp[3] << 8) | rsp[4];
            break;
        default:
            *value = 0;
    }
    return 1;
}
