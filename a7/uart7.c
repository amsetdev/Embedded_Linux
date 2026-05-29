#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

#define UART_DEV  "/dev/ttySTM0"
#define UART_BAUD B9600

int main(void)
{
    int fd = open(UART_DEV, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", UART_DEV, strerror(errno));
        return 1;
    }

    struct termios tty = {0};
    tcgetattr(fd, &tty);

    cfsetospeed(&tty, UART_BAUD);
    cfsetispeed(&tty, UART_BAUD);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_iflag = IGNBRK;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 5;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        return 1;
    }

    const char *msg = "Hello World\r\n";
    ssize_t n = write(fd, msg, strlen(msg));
    if (n < 0)
        fprintf(stderr, "write: %s\n", strerror(errno));
    else
        printf("Sent %zd bytes: %s", n, msg);

    close(fd);
    return 0;
}
