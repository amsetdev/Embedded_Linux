#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>

/* PE8 = ARD_D1 = UART7_TX  →  gpiochip4 (GPIOE), line 8 */
#define GPIO_CHIP   "/dev/gpiochip4"
#define GPIO_LINE   8          /* PE8 */
#define BAUD        9600
#define BIT_NS      (1000000000L / BAUD)

static int line_fd = -1;

static void pin_set(int v)
{
    struct gpiohandle_data data = {0};
    data.values[0] = v;
    ioctl(line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
}

static void bit_delay(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = BIT_NS };
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

static void uart_tx_byte(unsigned char b)
{
    pin_set(0);                      /* start bit */
    bit_delay();
    for (int i = 0; i < 8; i++) {   /* 8 data bits LSB first */
        pin_set((b >> i) & 1);
        bit_delay();
    }
    pin_set(1);                      /* stop bit */
    bit_delay();
}

int main(void)
{
    printf("Opening %s line %d (PE8/ARD_D1) @ %d baud\n",
           GPIO_CHIP, GPIO_LINE, BAUD);

    int chip_fd = open(GPIO_CHIP, O_RDONLY);
    if (chip_fd < 0) { perror("chip open"); return 1; }

    struct gpiohandle_request req = {0};
    req.lineoffsets[0]    = GPIO_LINE;
    req.flags             = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = 1;      /* idle HIGH */
    req.lines             = 1;
    strncpy(req.consumer_label, "uart_tx", sizeof(req.consumer_label)-1);

    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        fprintf(stderr, "GPIO request failed: %s\n", strerror(errno));
        close(chip_fd);
        return 1;
    }
    close(chip_fd);
    line_fd = req.fd;

    const char *msg = "Hello World\r\n";
    printf("Sending: %s", msg);
    fflush(stdout);

    for (const char *p = msg; *p; p++)
        uart_tx_byte((unsigned char)*p);

    close(line_fd);
    printf("Done — check PE8 (ARD_D1) pin with serial adapter or scope\n");
    return 0;
}
