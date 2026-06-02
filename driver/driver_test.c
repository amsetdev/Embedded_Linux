
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define SHARED_BASE  0x38000000UL
#define SHARED_MAGIC 0xABCD1234UL

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t a7_to_m4_head;
    uint32_t a7_to_m4_tail;
    uint32_t m4_to_a7_head;
    uint32_t m4_to_a7_tail;
    uint32_t m4_status;
    uint32_t reserved[2];
} SharedHeader;

#define HDR_SIZE     sizeof(SharedHeader)
#define BUF_SIZE     512

int main(void)
{
    /* ── Map RETRAM ─────────────────────────────────────────────────── */
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    size_t map_size = HDR_SIZE + BUF_SIZE * 2 + 4096;
    void *base = mmap(NULL, map_size, PROT_READ|PROT_WRITE,
                      MAP_SHARED, fd, SHARED_BASE);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    volatile SharedHeader *hdr     = (volatile SharedHeader *)base;
    volatile uint8_t *m4_to_a7    = (volatile uint8_t *)base + HDR_SIZE + BUF_SIZE;

    /* ── Step 1: write magic so driver opens ────────────────────────── */
    printf("Step 1: Writing magic 0x%08X to RETRAM...\n", SHARED_MAGIC);

    /* Zero everything first */
    memset((void*)base, 0, HDR_SIZE + BUF_SIZE * 2);

    hdr->a7_to_m4_head = 0;
    hdr->a7_to_m4_tail = 0;
    hdr->m4_to_a7_head = 0;
    hdr->m4_to_a7_tail = 0;
    hdr->m4_status     = 0;
    __sync_synchronize();
    hdr->magic = SHARED_MAGIC;
    __sync_synchronize();

    printf("Magic written: 0x%08x\n", hdr->magic);

    /* ── Step 2: fake a Modbus response in m4_to_a7 buffer ─────────── */
    /* Simulate M4 putting data: "Hello from M4!" */
    const char *test_data = "Hello_M4_Test!";
    uint16_t len = (uint16_t)strlen(test_data);

    printf("\nStep 2: Writing test data into m4_to_a7 buffer...\n");
    printf("  Data: \"%s\" (%d bytes)\n", test_data, len);

    for (int i = 0; i < len; i++)
        m4_to_a7[i & (BUF_SIZE-1)] = (uint8_t)test_data[i];

    __sync_synchronize();
    hdr->m4_to_a7_head = len;   /* driver sees this many bytes available   */
    __sync_synchronize();

    printf("  m4_to_a7_head = %d\n", hdr->m4_to_a7_head);
    printf("  m4_to_a7_tail = %d\n", hdr->m4_to_a7_tail);

    /* ── Step 3: now open /dev/modbus_uart and read ─────────────────── */
    printf("\nStep 3: Opening /dev/modbus_uart...\n");
    int dev = open("/dev/modbus_uart", O_RDONLY | O_NONBLOCK);
    if (dev < 0) { perror("open /dev/modbus_uart"); goto done; }
    printf("  Opened OK (fd=%d)\n", dev);

    char rbuf[64] = {0};
    int n = read(dev, rbuf, sizeof(rbuf)-1);
    if (n < 0) { perror("  read"); }
    else if (n == 0) { printf("  read: no data\n"); }
    else {
        rbuf[n] = '\0';
        printf("  Read %d bytes: \"%s\"\n", n, rbuf);
        printf("  SUCCESS — driver read/write working!\n");
    }
    close(dev);

    /* ── Step 4: test write direction ───────────────────────────────── */
    printf("\nStep 4: Testing write (A7→M4 direction)...\n");
    volatile uint8_t *a7_to_m4 = (volatile uint8_t *)base + HDR_SIZE;

    dev = open("/dev/modbus_uart", O_WRONLY | O_NONBLOCK);
    if (dev < 0) { perror("open /dev/modbus_uart for write"); goto done; }

    const char *wdata = "TestWrite";
    n = write(dev, wdata, strlen(wdata));
    if (n < 0) perror("  write");
    else printf("  Wrote %d bytes OK\n", n);

    /* Verify it landed in a7_to_m4 buffer */
    printf("  a7_to_m4_head = %d (should be %zu)\n",
           hdr->a7_to_m4_head, strlen(wdata));
    printf("  Buffer content: ");
    for (int i = 0; i < n && i < 16; i++)
        printf("%c", a7_to_m4[i]);
    printf("\n");

    if (hdr->a7_to_m4_head == (uint32_t)strlen(wdata))
        printf("  SUCCESS — write direction working!\n");

    close(dev);

done:
    munmap(base, map_size);
    close(fd);
    printf("\nDone.\n");
    return 0;
}
