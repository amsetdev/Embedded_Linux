/**
 * rtc.c
 * A7-side: reads system time (assumed NTP-synced), packs it, and writes it
 * to the M4 RETRAM command registers to set the DS3231 RTC.
 *
 * This is a module, not a standalone program — call rtc_main() from your
 * application (e.g. from a background thread in main.c). See rtc.h for
 * the return-code meanings.
 */
// run this to check the RTC registers on the M4 side:
//in hex:  devmem2 0x38000014 w ; devmem2 0x38000024 w ; devmem2 0x38000028 w
//In decimal:
//  watch -n 1 '
// hb=$(devmem2 0x38000014 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
// t=$(devmem2 0x38000024 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
// d=$(devmem2 0x38000028 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
// printf "HEARTBEAT : %d\n" "$hb"
// printf "TIME      : %02d:%02d:%02d\n" "$(( (t) >> 16 & 0xFF ))" "$(( (t) >> 8 & 0xFF ))" "$(( (t) & 0xFF ))"
// printf "DATE      : DOW=%d 20%02d-%02d-%02d\n" "$(( (d) >> 24 & 0xFF ))" "$(( (d) >> 16 & 0xFF ))" "$(( (d) >> 8 & 0xFF ))" "$(( (d) & 0xFF ))"
// '

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "rtc.h"

static volatile uint8_t *retram_map = NULL;

int map_retram(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return -1; }

    void *m = mmap(NULL, RETRAM_MAP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, RETRAM_PHYS_BASE);
    close(fd);

    if (m == MAP_FAILED) { perror("mmap"); return -1; }

    retram_map = (volatile uint8_t *)m;
    return 0;
}

void unmap_retram(void)
{
    if (retram_map != NULL)
    {
        munmap((void *)retram_map, RETRAM_MAP_SIZE);
        retram_map = NULL;
    }
}

void reg_write32(uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(retram_map + offset) = val;
}

uint32_t reg_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(retram_map + offset);
}

/* Returns 1 if internet is reachable, 0 otherwise. Uses ping via fork/exec
   rather than system() for slightly safer error handling. */
int check_internet(void)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execlp("ping", "ping", "-c", "1", "-W", "2", "8.8.8.8", (char *)NULL);
        _exit(127);
    }
    else if (pid > 0)
    {
        int status;
        waitpid(pid, &status, 0);
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
    }
    return 0;
}

/* Runs ntpd -q once to force a one-shot sync. Ignores failure since the
   system clock may already be synced by a background ntpd/chrony. */
void sync_ntp(void)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execlp("ntpd", "ntpd", "-q", "-p", "pool.ntp.org", (char *)NULL);
        _exit(127);
    }
    else if (pid > 0)
    {
        int status;
        waitpid(pid, &status, 0);
    }
}

int rtc_main(void)
{
    if (!check_internet())
    {
        fprintf(stderr, "No internet - skipping RTC sync.\n");
        return 1;
    }

    sync_ntp();

    time_t now = time(NULL);
    struct tm tmval;
    if (localtime_r(&now, &tmval) == NULL)
    {
        fprintf(stderr, "localtime_r failed\n");
        return 1;
    }

    uint8_t sec   = (uint8_t)tmval.tm_sec;
    uint8_t min_  = (uint8_t)tmval.tm_min;
    uint8_t hour  = (uint8_t)tmval.tm_hour;
    uint8_t day   = (uint8_t)tmval.tm_mday;
    uint8_t month = (uint8_t)(tmval.tm_mon + 1);
    uint8_t year  = (uint8_t)(tmval.tm_year + 1900 - 2000);
    uint8_t dow   = (uint8_t)(tmval.tm_wday == 0 ? 7 : tmval.tm_wday);

    uint32_t time_packed = ((uint32_t)hour << 16) | ((uint32_t)min_ << 8) | sec;
    uint32_t date_packed = ((uint32_t)dow << 24) | ((uint32_t)year << 16) |
                            ((uint32_t)month << 8) | day;

    printf("System time: 20%02u-%02u-%02u %02u:%02u:%02u (DOW=%u)\n",
           year, month, day, hour, min_, sec, dow);
    printf("Packed: time=0x%08X date=0x%08X\n", time_packed, date_packed);

    if (map_retram() != 0)
    {
        return 2;
    }

    reg_write32(OFF_CMD_TIME, time_packed);
    reg_write32(OFF_CMD_DATE, date_packed);
    reg_write32(OFF_CMD_FLAG, 1U);

    for (int i = 0; i < ACK_POLL_ATTEMPTS; i++)
    {
        sleep(ACK_POLL_DELAY_SEC);
        uint32_t ack = reg_read32(OFF_CMD_ACK);
        if (ack == 1U)
        {
            printf("RTC set confirmed by M4.\n");
            reg_write32(OFF_CMD_ACK, 0U);
            unmap_retram();
            return 0;
        }
    }

    fprintf(stderr, "M4 did not acknowledge time set - check firmware/connection.\n");
    unmap_retram();
    return 3;
}