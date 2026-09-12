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

#define RS485_GPIOCHIP  "/dev/gpiochip4"
#define RS485_GPIO_LINE 10

static int gpio_fd   = -1;
static int gpio_line = -1;

int uart_fd = -1;  /**< Raw UART file descriptor. */

/**
 * @brief Initializes the RS-485 Driver Enable (DE) GPIO.
 *
 * Opens the GPIO chip and configures the DE pin as an output.
 * The pin is initially driven LOW to place the transceiver in
 * receive mode.
 *
 * @return 0 on success, -1 on failure.
 */
int rs485_gpio_init(void)
{
    gpio_fd = open(RS485_GPIOCHIP, O_RDONLY);
    if (gpio_fd < 0) { perror("open /dev/gpiochip4"); return -1; }

    struct gpiohandle_request req;
    memset(&req, 0, sizeof(req));
    req.lineoffsets[0]    = RS485_GPIO_LINE;
    req.lines             = 1;
    req.flags             = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = 0;
    strncpy(req.consumer_label, "modbus_de", sizeof(req.consumer_label) - 1);

    if (ioctl(gpio_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        perror("GPIO_GET_LINEHANDLE_IOCTL");
        close(gpio_fd);
        gpio_fd = -1;
        return -1;
    }

    gpio_line = req.fd;
    printf("[RS485] PE10 init OK — LOW=RX ready\n");
    return 0;
}

/**
 * @brief Enables RS-485 transmit mode.
 *
 * Sets the Driver Enable (DE) pin HIGH and waits for the
 * configured pre-transmit delay.
 */
void rs485_tx(void)
{
    if (gpio_line < 0)
        return;

    struct gpiohandle_data d;
    memset(&d, 0, sizeof(d));
    d.values[0] = 1;

    ioctl(gpio_line, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d);
    usleep(RS485_PRE_TX_US);
}

/**
 * @brief Enables RS-485 receive mode.
 *
 * Clears the Driver Enable (DE) pin, allowing the transceiver
 * to receive data.
 */
void rs485_rx(void)
{
    if (gpio_line < 0)
        return;

    struct gpiohandle_data d;
    memset(&d, 0, sizeof(d));
    d.values[0] = 0;

    ioctl(gpio_line, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d);
}

/**
 * @brief Releases RS-485 GPIO resources.
 *
 * Closes the GPIO line handle and GPIO chip device.
 */
void rs485_gpio_close(void)
{
    if (gpio_line >= 0) {
        close(gpio_line);
        gpio_line = -1;
    }

    if (gpio_fd >= 0) {
        close(gpio_fd);
        gpio_fd = -1;
    }
}

/**
 * @brief Opens and configures the UART port.
 *
 * Configures the serial port for 8 data bits, no parity,
 * one stop bit (8N1), and the specified baud rate.
 *
 * @param port UART device path.
 * @param baud Communication baud rate.
 *
 * @return 0 on success, -1 on failure.
 */
int uart_open(const char *port, int baud)
{
    uart_fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (uart_fd < 0) {
        perror("uart_open");
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    if (tcgetattr(uart_fd, &tty) < 0) {
        perror("tcgetattr");
        return -1;
    }

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

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_iflag = IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(uart_fd, TCSANOW, &tty) < 0) {
        perror("tcsetattr");
        return -1;
    }

    tcflush(uart_fd, TCIOFLUSH);
    return 0;
}

/**
 * @brief Closes the UART port.
 */
void uart_close(void)
{
    if (uart_fd >= 0) {
        close(uart_fd);
        uart_fd = -1;
    }
}

/**
 * @brief Waits until all UART transmit data has been sent.
 *
 * Ensures the UART hardware shift register is empty before
 * switching the RS-485 transceiver back to receive mode.
 */
void uart_drain_tx(void)
{
    if (uart_fd < 0)
        return;

    tcdrain(uart_fd);
    usleep(RS485_TX_GUARD_US);
}

/**
 * @brief Flushes all pending received UART data.
 */
void uart_flush_rx(void)
{
    if (uart_fd < 0)
        return;

    tcflush(uart_fd, TCIFLUSH);
}

/**
 * @brief Reads data from the UART with a timeout.
 *
 * Attempts to read the requested number of bytes before the
 * specified timeout expires.
 *
 * @param buf Buffer to store received data.
 * @param want Number of bytes to read.
 * @param timeout_ms Timeout in milliseconds.
 *
 * @return Number of bytes actually read.
 */
int uart_read_timeout(uint8_t *buf, int want, int timeout_ms)
{
    if (uart_fd < 0 || buf == NULL || want <= 0)
        return -1;

    int total = 0;

    struct timeval start;
    gettimeofday(&start, NULL);

    while (total < want)
    {
        struct timeval now;

        gettimeofday(&now, NULL);

        long elapsed_ms =
            (now.tv_sec - start.tv_sec) * 1000L +
            (now.tv_usec - start.tv_usec) / 1000L;

        int remaining_ms =
            timeout_ms - (int)elapsed_ms;

        if (remaining_ms <= 0)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(uart_fd, &rfds);

        struct timeval tv;

        tv.tv_sec = remaining_ms / 1000;
        tv.tv_usec = (remaining_ms % 1000) * 1000;

        int ret = select(uart_fd + 1,
                         &rfds,
                         NULL,
                         NULL,
                         &tv);

        if (ret < 0)
        {
            if (errno == EINTR)
                continue;

            perror("[UART] select");
            return -1;
        }

        if (ret == 0)
            break;

        int n = read(uart_fd,
                     buf + total,
                     want - total);

        if (n > 0)
        {
            total += n;
            continue;
        }

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            perror("[UART] read");
            return -1;
        }
    }

    return total;
}

/**
 * @brief Calculates the Modbus RTU CRC-16 checksum.
 *
 * @param buf Pointer to the data buffer.
 * @param len Number of bytes in the buffer.
 *
 * @return Calculated CRC-16 value.
 */
uint16_t mb_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;

    for (int pos = 0; pos < len; pos++)
    {
        crc ^= buf[pos];

        for (int i = 0; i < 8; i++)
        {
            if (crc & 0x0001)
            {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc;
}

/**
 * @brief Returns the expected reply length for a Modbus function code.
 *
 * @param fc Modbus function code.
 *
 * @return Expected response length in bytes.
 */
int mb_reply_len(uint8_t fc)
{
    switch (fc)
    {
        case 0x01:
        case 0x02:
            /*
             * For a single coil/input:
             *
             * slave + fc + byte_count + data + CRC
             *
             * = 6 bytes
             */
            return 6;

        case 0x03:
        case 0x04:
            /*
             * One 16-bit register:
             *
             * slave + fc + byte_count + 2 data bytes + CRC
             *
             * = 7 bytes
             */
            return 7;

        default:
            return -1;
    }
}
/**
 * @brief Performs a Modbus RTU transaction.
 *
 * Builds and transmits a Modbus request frame, waits for the
 * response, validates the CRC and response fields, and extracts
 * the returned value.
 *
 * Supported function codes:
 * - 0x01 Read Coils
 * - 0x02 Read Discrete Inputs
 * - 0x03 Read Holding Registers
 * - 0x04 Read Input Registers
 *
 * @param slave Modbus slave address.
 * @param fc Modbus function code.
 * @param addr Register or coil address.
 * @param value Pointer to store the received value.
 *
 * @return 1 on success, 0 on communication or validation failure.
 */
int mb_transaction(uint8_t slave,
                   uint8_t fc,
                   uint16_t addr,
                   uint16_t *value)
{
    if (uart_fd < 0 || value == NULL)
        return 0;

    uint8_t tx[8];
    uint8_t rx[256];

    int tx_len = 0;

    /*
     * Modbus RTU request:
     *
     * [Slave]
     * [Function]
     * [Address Hi]
     * [Address Lo]
     * [Quantity Hi]
     * [Quantity Lo]
     * [CRC Lo]
     * [CRC Hi]
     */

    tx[tx_len++] = slave;
    tx[tx_len++] = fc;

    tx[tx_len++] = (uint8_t)(addr >> 8);
    tx[tx_len++] = (uint8_t)(addr & 0xFF);

    /*
     * Read one coil/register/input.
     */
    tx[tx_len++] = 0x00;
    tx[tx_len++] = 0x01;

    uint16_t crc = mb_crc16(tx, tx_len);

    tx[tx_len++] = (uint8_t)(crc & 0xFF);
    tx[tx_len++] = (uint8_t)(crc >> 8);


    /*
     * Remove stale data from previous transaction.
     */
    uart_flush_rx();


    /*
     * Switch RS485 transceiver to TX.
     */
    rs485_tx();


    /*
     * Send complete frame.
     */
    ssize_t written =
        write(uart_fd,
              tx,
              tx_len);

    if (written != tx_len)
    {
        perror("[MB] write");

        uart_drain_tx();
        rs485_rx();

        return 0;
    }


    /*
     * Wait until UART hardware has physically transmitted
     * the complete frame.
     */
    uart_drain_tx();


    /*
     * Switch RS485 transceiver to RX.
     */
    rs485_rx();


    /*
     * Expected response.
     */
    int expected = mb_reply_len(fc);

    if (expected < 0)
        return 0;


    /*
     * Give the slave time to respond.
     */
    int n =
        uart_read_timeout(rx,
                          expected,
                          500);


    if (n < expected)
    {
        printf("[MB] Timeout: received %d/%d bytes\n",
               n,
               expected);

        return 0;
    }


    /*
     * Check slave address.
     */
    if (rx[0] != slave)
    {
        printf("[MB] Invalid slave response: %02X\n",
               rx[0]);

        return 0;
    }


    /*
     * Check exception response.
     *
     * Exception function = requested function | 0x80
     */
    if (rx[1] == (uint8_t)(fc | 0x80))
    {
        printf("[MB] Modbus exception: code=%02X\n",
               rx[2]);

        return 0;
    }


    /*
     * Check function code.
     */
    if (rx[1] != fc)
    {
        printf("[MB] Invalid function: expected=%02X got=%02X\n",
               fc,
               rx[1]);

        return 0;
    }


    /*
     * Validate response length according to byte count.
     */
    if (fc == 0x03 || fc == 0x04)
    {
        if (rx[2] != 2)
        {
            printf("[MB] Invalid byte count: %d\n",
                   rx[2]);

            return 0;
        }

        /*
         * Extract 16-bit register value.
         *
         * Modbus uses big-endian register data.
         */
        *value =
            ((uint16_t)rx[3] << 8) |
            rx[4];
    }
    else if (fc == 0x01 || fc == 0x02)
    {
        if (rx[2] != 1)
        {
            printf("[MB] Invalid byte count: %d\n",
                   rx[2]);

            return 0;
        }

        /*
         * First coil/input bit.
         */
        *value =
            (rx[3] & 0x01) ? 1 : 0;
    }


    /*
     * CRC check.
     */
    uint16_t received_crc =
        ((uint16_t)rx[n - 1] << 8) |
        rx[n - 2];

    uint16_t calculated_crc =
        mb_crc16(rx, n - 2);

    if (received_crc != calculated_crc)
    {
        printf("[MB] CRC error: received=%04X calculated=%04X\n",
               received_crc,
               calculated_crc);

        return 0;
    }


    return 1;
}

// /**
//  * @brief Writes one Modbus holding register.
//  *
//  * Uses Modbus RTU Function Code 06.
//  *
//  * Request:
//  *   [Slave]
//  *   [06]
//  *   [Address Hi]
//  *   [Address Lo]
//  *   [Value Hi]
//  *   [Value Lo]
//  *   [CRC Lo]
//  *   [CRC Hi]
//  *
//  * Response:
//  *   Same frame is echoed by the slave.
//  *
//  * @return 1 on success, 0 on failure.
//  */
// int mb_write_register(uint8_t slave,
//                       uint16_t addr,
//                       uint16_t value)
// {
//     if (uart_fd < 0)
//         return 0;

//     uint8_t tx[8];
//     uint8_t rx[8];

//     int tx_len = 0;

//     /*
//      * ------------------------------------------------------------
//      * Build Modbus Function 06 request
//      * ------------------------------------------------------------
//      */

//     tx[tx_len++] = slave;
//     tx[tx_len++] = 0x06;

//     /* Register address */
//     tx[tx_len++] = (uint8_t)(addr >> 8);
//     tx[tx_len++] = (uint8_t)(addr & 0xFF);

//     /* Register value */
//     tx[tx_len++] = (uint8_t)(value >> 8);
//     tx[tx_len++] = (uint8_t)(value & 0xFF);

//     /* CRC */
//     uint16_t crc = mb_crc16(tx, tx_len);

//     tx[tx_len++] = (uint8_t)(crc & 0xFF);
//     tx[tx_len++] = (uint8_t)(crc >> 8);

//     /*
//      * ------------------------------------------------------------
//      * Remove stale RX data
//      * ------------------------------------------------------------
//      */

//     uart_flush_rx();

//     /*
//      * ------------------------------------------------------------
//      * Enable RS-485 transmit mode
//      * ------------------------------------------------------------
//      */

//     rs485_tx();

//     /*
//      * ------------------------------------------------------------
//      * Send request
//      * ------------------------------------------------------------
//      */

//     ssize_t written = write(uart_fd, tx, tx_len);

//     if (written != tx_len)
//     {
//         perror("[MB WRITE] write");

//         uart_drain_tx();
//         rs485_rx();

//         return 0;
//     }

//     /*
//      * ------------------------------------------------------------
//      * Wait until complete frame has physically left UART
//      * ------------------------------------------------------------
//      */

//     uart_drain_tx();

//     /*
//      * ------------------------------------------------------------
//      * Switch RS-485 back to receive mode
//      * ------------------------------------------------------------
//      */

//     rs485_rx();

//     /*
//      * ------------------------------------------------------------
//      * Function 06 response is exactly 8 bytes
//      *
//      * Slave + FC + Address(2) + Value(2) + CRC(2)
//      * ------------------------------------------------------------
//      */

//     int expected = 8;

//     int n = uart_read_timeout(rx,
//                               expected,
//                               RX_TIMEOUT_MS);

//     if (n < expected)
//     {
//         printf("[MB WRITE] Timeout: received %d/%d bytes\n",
//                n,
//                expected);

//         return 0;
//     }

//     /*
//      * ------------------------------------------------------------
//      * Check slave address
//      * ------------------------------------------------------------
//      */

//     if (rx[0] != slave)
//     {
//         printf("[MB WRITE] Invalid slave response: %02X\n",
//                rx[0]);

//         return 0;
//     }

//     /*
//      * ------------------------------------------------------------
//      * Check exception response
//      *
//      * For FC 06:
//      * Exception FC = 0x06 | 0x80 = 0x86
//      * ------------------------------------------------------------
//      */

//     if (rx[1] == 0x86)
//     {
//         printf("[MB WRITE] Modbus exception: code=%02X\n",
//                rx[2]);

//         return 0;
//     }

//     /*
//      * ------------------------------------------------------------
//      * Check function code
//      * ------------------------------------------------------------
//      */

//     if (rx[1] != 0x06)
//     {
//         printf("[MB WRITE] Invalid function: expected=06 got=%02X\n",
//                rx[1]);

//         return 0;
//     }

//     /*
//      * ------------------------------------------------------------
//      * Validate echoed address
//      * ------------------------------------------------------------
//      */

//     uint16_t response_addr =
//         ((uint16_t)rx[2] << 8) |
//         rx[3];

//     if (response_addr != addr)
//     {
//         printf("[MB WRITE] Address mismatch: expected=%04X got=%04X\n",
//                addr,
//                response_addr);

//         return 0;
//     }

//     /*
//      * ------------------------------------------------------------
//      * Validate echoed value
//      * ------------------------------------------------------------
//      */

//     uint16_t response_value =
//         ((uint16_t)rx[4] << 8) |
//         rx[5];

//     if (response_value != value)
//     {
//         printf("[MB WRITE] Value mismatch: expected=%04X got=%04X\n",
//                value,
//                response_value);

//         return 0;
//     }

//     /*
//      * ------------------------------------------------------------
//      * CRC check
//      * ------------------------------------------------------------
//      */

//     uint16_t received_crc =
//         ((uint16_t)rx[7] << 8) |
//         rx[6];

//     uint16_t calculated_crc =
//         mb_crc16(rx, 6);

//     if (received_crc != calculated_crc)
//     {
//         printf("[MB WRITE] CRC error: received=%04X calculated=%04X\n",
//                received_crc,
//                calculated_crc);

//         return 0;
//     }

//     printf("[MB WRITE] Slave=%u Addr=%u Value=%u SUCCESS\n",
//            slave,
//            addr,
//            value);

//     return 1;
// }


int mb_write_registers(uint8_t slave,
                       uint16_t start_addr,
                       const uint16_t *values,
                       uint16_t quantity)
{
    if (uart_fd < 0 || values == NULL || quantity == 0)
        return 0;

    if (quantity > 123)
        return 0;

    uint8_t tx[256];
    uint8_t rx[8];

    int tx_len = 0;

    /* ------------------------------------------------------------ */
    /* Build FC10 request                                           */
    /* ------------------------------------------------------------ */

    tx[tx_len++] = slave;
    tx[tx_len++] = 0x10;

    /* Starting address */
    tx[tx_len++] = (uint8_t)(start_addr >> 8);
    tx[tx_len++] = (uint8_t)(start_addr & 0xFF);

    /* Number of registers */
    tx[tx_len++] = (uint8_t)(quantity >> 8);
    tx[tx_len++] = (uint8_t)(quantity & 0xFF);

    /* Byte count */
    tx[tx_len++] = (uint8_t)(quantity * 2);

    /* Register data */
    for (uint16_t i = 0; i < quantity; i++)
    {
        tx[tx_len++] = (uint8_t)(values[i] >> 8);
        tx[tx_len++] = (uint8_t)(values[i] & 0xFF);
    }

    /* CRC */
    uint16_t crc = mb_crc16(tx, tx_len);

    tx[tx_len++] = (uint8_t)(crc & 0xFF);
    tx[tx_len++] = (uint8_t)(crc >> 8);

    /* ------------------------------------------------------------ */
    /* Debug TX frame                                               */
    /* ------------------------------------------------------------ */

    printf("[MB WRITE MULTI TX] ");

    for (int i = 0; i < tx_len; i++)
        printf("%02X ", tx[i]);

    printf("\n");

    /* ------------------------------------------------------------ */
    /* Clear old RX data                                            */
    /* ------------------------------------------------------------ */

    uart_flush_rx();

    /* ------------------------------------------------------------ */
    /* RS485 TX                                                      */
    /* ------------------------------------------------------------ */

    rs485_tx();

    ssize_t written = write(uart_fd, tx, tx_len);

    if (written != tx_len)
    {
        perror("[MB WRITE MULTI] write");

        uart_drain_tx();
        rs485_rx();

        return 0;
    }

    /* Wait for physical transmission to complete */
    uart_drain_tx();

    /* RS485 RX */
    rs485_rx();

    /* ------------------------------------------------------------ */
    /* FC10 response = 8 bytes                                      */
    /* ------------------------------------------------------------ */

    int expected = 8;

    int n = uart_read_timeout(rx,
                              expected,
                              RX_TIMEOUT_MS);

    if (n < expected)
    {
        printf("[MB WRITE MULTI] Timeout: received %d/%d bytes\n",
               n,
               expected);

        return 0;
    }

    /* ------------------------------------------------------------ */
    /* Debug RX frame                                               */
    /* ------------------------------------------------------------ */

    printf("[MB WRITE MULTI RX] ");

    for (int i = 0; i < n; i++)
        printf("%02X ", rx[i]);

    printf("\n");

    /* ------------------------------------------------------------ */
    /* Slave check                                                   */
    /* ------------------------------------------------------------ */

    if (rx[0] != slave)
    {
        printf("[MB WRITE MULTI] Invalid slave: %02X\n",
               rx[0]);

        return 0;
    }

    /* ------------------------------------------------------------ */
    /* Exception response                                           */
    /* ------------------------------------------------------------ */

    if (rx[1] == 0x90)
    {
        printf("[MB WRITE MULTI] Modbus exception: code=%02X\n",
               rx[2]);

        return 0;
    }

    /* ------------------------------------------------------------ */
    /* Function check                                               */
    /* ------------------------------------------------------------ */

    if (rx[1] != 0x10)
    {
        printf("[MB WRITE MULTI] Invalid function: expected=10 got=%02X\n",
               rx[1]);

        return 0;
    }

    /* ------------------------------------------------------------ */
    /* Check starting address                                       */
    /* ------------------------------------------------------------ */

    uint16_t response_addr =
        ((uint16_t)rx[2] << 8) |
        rx[3];

    if (response_addr != start_addr)
    {
        printf("[MB WRITE MULTI] Address mismatch: expected=%04X got=%04X\n",
               start_addr,
               response_addr);

        return 0;
    }

    /* ------------------------------------------------------------ */
    /* Check quantity                                               */
    /* ------------------------------------------------------------ */

    uint16_t response_quantity =
        ((uint16_t)rx[4] << 8) |
        rx[5];

    if (response_quantity != quantity)
    {
        printf("[MB WRITE MULTI] Quantity mismatch: expected=%u got=%u\n",
               quantity,
               response_quantity);

        return 0;
    }

    /* ------------------------------------------------------------ */
    /* CRC check                                                     */
    /* ------------------------------------------------------------ */

    uint16_t received_crc =
        ((uint16_t)rx[7] << 8) |
        rx[6];

    uint16_t calculated_crc =
        mb_crc16(rx, 6);

    if (received_crc != calculated_crc)
    {
        printf("[MB WRITE MULTI] CRC error: received=%04X calculated=%04X\n",
               received_crc,
               calculated_crc);

        return 0;
    }

    printf("[MB WRITE MULTI] Slave=%u Start=%u Quantity=%u SUCCESS\n",
           slave,
           start_addr,
           quantity);

    return 1;
}