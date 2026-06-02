/**
 ******************************************************************************
 * @file    modbus_pty_bridge.c
 * @brief   PTY bridge — creates /dev/modbus_uart as a real tty device
 *
 *  Creates a PTY pair:
 *    - Slave end symlinked to /dev/modbus_uart  ← any app opens this
 *    - Master end bridges to RETRAM shared buffer ← talks to M4
 *
 *  Any software (libmodbus, minicom, pyserial) opens /dev/modbus_uart
 *  exactly like a real serial port — tcsetattr, baud rate, all work.
 *
 *  Build:
 *    gcc -O2 -o modbus_pty_bridge modbus_pty_bridge.c
 *
 *  Run (as root, before your app):
 *    ./modbus_pty_bridge &
 *
 *  Then your app:
 *    modbus_new_rtu("/dev/modbus_uart", 9600, 'N', 8, 1)  ← works!
 ******************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <pty.h>         /* openpty() */
#include <termios.h>

/* ── Shared RETRAM buffer — must match M4 firmware and kernel driver ─────── */
#define SHARED_BASE      0x38000000UL
#define SHARED_MAGIC     0xABCD1234UL
#define CIRC_BUF_SIZE    512U
#define CIRC_BUF_MASK    (CIRC_BUF_SIZE - 1U)
#define CIRC_COUNT(h,t)  ((h)-(t))
#define CIRC_SPACE(h,t)  (CIRC_BUF_SIZE-((h)-(t)))
#define CIRC_IDX(x)      ((x)&CIRC_BUF_MASK)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t a7_to_m4_head;
    uint32_t a7_to_m4_tail;
    uint32_t m4_to_a7_head;
    uint32_t m4_to_a7_tail;
    uint32_t m4_status;
    uint32_t reserved[2];
} SharedHeader;

#define HDR_SIZE          sizeof(SharedHeader)
#define A7_TO_M4_OFFSET   HDR_SIZE
#define M4_TO_A7_OFFSET   (HDR_SIZE + CIRC_BUF_SIZE)
#define MAP_SIZE          (HDR_SIZE + CIRC_BUF_SIZE * 2 + 4096)

#define SYMLINK_PATH      "/dev/modbus_uart"

static volatile int running = 1;
static void on_signal(int s) { (void)s; running = 0; }

int main(void)
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    printf("[PTY Bridge] Starting modbus_pty_bridge\n");

    /* ── Map RETRAM ──────────────────────────────────────────────────── */
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("open /dev/mem"); return 1; }

    void *base = mmap(NULL, MAP_SIZE, PROT_READ|PROT_WRITE,
                      MAP_SHARED, memfd, SHARED_BASE);
    if (base == MAP_FAILED) { perror("mmap RETRAM"); return 1; }

    volatile SharedHeader *hdr     = (volatile SharedHeader *)base;
    volatile uint8_t *a7_to_m4    = (volatile uint8_t *)base + A7_TO_M4_OFFSET;
    volatile uint8_t *m4_to_a7    = (volatile uint8_t *)base + M4_TO_A7_OFFSET;

    /* ── Wait for M4 magic ───────────────────────────────────────────── */
    printf("[PTY Bridge] Waiting for M4 magic at 0x%08lX...\n", SHARED_BASE);
    while (running) {
        if (hdr->magic == SHARED_MAGIC) break;
        usleep(50000);
    }
    if (!running) { munmap(base, MAP_SIZE); close(memfd); return 0; }
    printf("[PTY Bridge] M4 ready!\n");

    /* ── Create PTY pair ─────────────────────────────────────────────── */
    int master_fd, slave_fd;
    char slave_name[64];

    if (openpty(&master_fd, &slave_fd, slave_name, NULL, NULL) < 0) {
        perror("openpty"); return 1;
    }

    /* Set master non-blocking */
    fcntl(master_fd, F_SETFL, O_NONBLOCK);

    /* Configure slave tty — raw mode so bytes pass through unchanged */
    struct termios tio;
    tcgetattr(slave_fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);
    tcsetattr(slave_fd, TCSANOW, &tio);

    /* Remove old symlink and create new one */
    unlink(SYMLINK_PATH);
    if (symlink(slave_name, SYMLINK_PATH) < 0) {
        perror("symlink"); return 1;
    }

    printf("[PTY Bridge] PTY slave: %s\n", slave_name);
    printf("[PTY Bridge] Symlink:   %s -> %s\n", SYMLINK_PATH, slave_name);
    printf("[PTY Bridge] Ready — open %s like a real serial port\n\n",
           SYMLINK_PATH);

    /* ── Bridge loop ─────────────────────────────────────────────────── */
    uint8_t buf[256];

    while (running)
    {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 5000 };  /* 5ms */
        FD_ZERO(&rfds);
        FD_SET(master_fd, &rfds);
        select(master_fd + 1, &rfds, NULL, NULL, &tv);

        /* ── PTY master → RETRAM a7_to_m4 (app wrote a Modbus request) */
        if (FD_ISSET(master_fd, &rfds)) {
            int n = read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                uint32_t head = hdr->a7_to_m4_head;
                uint32_t tail = hdr->a7_to_m4_tail;
                uint32_t space = CIRC_SPACE(head, tail);
                uint32_t write_len = (n < (int)space) ? n : (int)space;

                for (uint32_t i = 0; i < write_len; i++)
                    a7_to_m4[CIRC_IDX(head + i)] = buf[i];

                __sync_synchronize();
                hdr->a7_to_m4_head = head + write_len;
                __sync_synchronize();

                printf("[PTY Bridge] App→M4: %d bytes\n", n);
            }
        }

        /* ── RETRAM m4_to_a7 → PTY master (M4 got response from slave) */
        {
            uint32_t head = hdr->m4_to_a7_head;
            uint32_t tail = hdr->m4_to_a7_tail;
            uint32_t avail = CIRC_COUNT(head, tail);

            if (avail > 0) {
                uint32_t read_len = avail < sizeof(buf) ? avail : sizeof(buf);

                for (uint32_t i = 0; i < read_len; i++)
                    buf[i] = m4_to_a7[CIRC_IDX(tail + i)];

                __sync_synchronize();
                hdr->m4_to_a7_tail = tail + read_len;
                __sync_synchronize();

                /* Write response back to app via PTY master */
                write(master_fd, buf, read_len);
                printf("[PTY Bridge] M4→App: %d bytes\n", (int)read_len);
            }
        }
    }

    /* ── Cleanup ─────────────────────────────────────────────────────── */
    printf("\n[PTY Bridge] Shutting down...\n");
    unlink(SYMLINK_PATH);
    close(slave_fd);
    close(master_fd);
    munmap(base, MAP_SIZE);
    close(memfd);
    return 0;
}