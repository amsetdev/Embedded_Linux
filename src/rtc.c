/**
 * @file rtc.c
 * @brief RTC synchronization module for STM32MP1 A7 core.
 *
 * This module synchronizes the Linux system time with an external
 * DS3231 RTC connected to the Cortex-M4 core.
 *
 * Workflow:
 * - Checks for Internet connectivity.
 * - Performs a one-time NTP synchronization.
 * - Reads the current Linux system time.
 * - Packs the time and date into RETRAM command registers.
 * - Signals the Cortex-M4 firmware to update the DS3231 RTC.
 * - Waits for an acknowledgement from the Cortex-M4.
 *
 * The Linux system clock is assumed to be synchronized using NTP.
 * This file is intended to be used as a module. Call rtc_main()
 * from the application whenever the RTC needs to be synchronized.
 *
 * RETRAM register layout:
 * - OFF_CMD_TIME : Packed HH:MM:SS
 * - OFF_CMD_DATE : Packed DOW:YY:MM:DD
 * - OFF_CMD_FLAG : Command flag
 * - OFF_CMD_ACK  : Acknowledgement flag
 */

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

/**
 * @brief Pointer to the mapped RETRAM memory region.
 */
static volatile uint8_t *retram_map = NULL;

/**
 * @brief Maps the Cortex-M4 RETRAM into the Linux virtual address space.
 *
 * Opens /dev/mem and maps the shared RETRAM region used for
 * communication between the Cortex-A7 and Cortex-M4 cores.
 *
 * @return
 * - 0 on success.
 * - -1 if the memory cannot be mapped.
 */
int map_retram(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0)
    {
        perror("open /dev/mem");
        return -1;
    }

    void *m = mmap(NULL,
                   RETRAM_MAP_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   fd,
                   RETRAM_PHYS_BASE);

    close(fd);

    if (m == MAP_FAILED)
    {
        perror("mmap");
        return -1;
    }

    retram_map = (volatile uint8_t *)m;

    return 0;
}

/**
 * @brief Unmaps the RETRAM memory region.
 *
 * Releases the mapped RETRAM memory if it has been mapped.
 */
void unmap_retram(void)
{
    if (retram_map != NULL)
    {
        munmap((void *)retram_map, RETRAM_MAP_SIZE);
        retram_map = NULL;
    }
}

/**
 * @brief Writes a 32-bit value to a RETRAM register.
 *
 * @param offset Register offset from the RETRAM base address.
 * @param val Value to write.
 */
void reg_write32(uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)(retram_map + offset) = val;
}

/**
 * @brief Reads a 32-bit value from a RETRAM register.
 *
 * @param offset Register offset from the RETRAM base address.
 *
 * @return Register value.
 */
uint32_t reg_read32(uint32_t offset)
{
    return *(volatile uint32_t *)(retram_map + offset);
}

/**
 * @brief Checks whether Internet connectivity is available.
 *
 * Executes a single ping request to Google's public DNS server
 * (8.8.8.8). Output is redirected to /dev/null.
 *
 * @return
 * - 1 if the host is reachable.
 * - 0 otherwise.
 */
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

        execlp("ping",
               "ping",
               "-c", "1",
               "-W", "2",
               "8.8.8.8",
               (char *)NULL);

        _exit(127);
    }
    else if (pid > 0)
    {
        int status;

        waitpid(pid, &status, 0);

        return (WIFEXITED(status) &&
                WEXITSTATUS(status) == 0);
    }

    return 0;
}

/**
 * @brief Performs a one-shot NTP synchronization.
 *
 * Executes ntpd in one-shot mode to synchronize the Linux system
 * clock with an NTP server.
 *
 * Any failure is ignored because another time synchronization
 * service may already be running.
 */
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

        execlp("ntpd",
               "ntpd",
               "-q",
               "-p",
               "pool.ntp.org",
               (char *)NULL);

        _exit(127);
    }
    else if (pid > 0)
    {
        int status;
        waitpid(pid, &status, 0);
    }
}

/**
 * @brief Synchronizes the DS3231 RTC using the Linux system time.
 *
 * The function performs the following operations:
 * - Verifies Internet connectivity.
 * - Synchronizes the Linux system clock using NTP.
 * - Reads the current system date and time.
 * - Packs the date and time into RETRAM command registers.
 * - Signals the Cortex-M4 firmware to update the DS3231 RTC.
 * - Waits for an acknowledgement from the Cortex-M4.
 *
 * @return
 * - 0 : RTC successfully updated.
 * - 1 : Internet unavailable or time conversion failed.
 * - 2 : RETRAM mapping failed.
 * - 3 : Cortex-M4 acknowledgement timeout.
 */
int rtc_main(void)
{
    if (!check_internet())
    {
        fprintf(stderr,
                "No internet - skipping RTC sync.\n");
        return 1;
    }

    sync_ntp();

    time_t now = time(NULL);

    struct tm tmval;

    if (localtime_r(&now, &tmval) == NULL)
    {
        fprintf(stderr,
                "localtime_r failed\n");
        return 1;
    }

    uint8_t sec   = (uint8_t)tmval.tm_sec;
    uint8_t min_  = (uint8_t)tmval.tm_min;
    uint8_t hour  = (uint8_t)tmval.tm_hour;
    uint8_t day   = (uint8_t)tmval.tm_mday;
    uint8_t month = (uint8_t)(tmval.tm_mon + 1);
    uint8_t year  = (uint8_t)(tmval.tm_year + 1900 - 2000);
    uint8_t dow   = (uint8_t)(tmval.tm_wday == 0 ? 7 : tmval.tm_wday);

    uint32_t time_packed =
        ((uint32_t)hour << 16) |
        ((uint32_t)min_ << 8) |
        sec;

    uint32_t date_packed =
        ((uint32_t)dow << 24) |
        ((uint32_t)year << 16) |
        ((uint32_t)month << 8) |
        day;

    printf("System time: 20%02u-%02u-%02u %02u:%02u:%02u (DOW=%u)\n",
           year,
           month,
           day,
           hour,
           min_,
           sec,
           dow);

    printf("Packed: time=0x%08X date=0x%08X\n",
           time_packed,
           date_packed);

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

    fprintf(stderr,
            "M4 did not acknowledge time set - check firmware/connection.\n");

    unmap_retram();

    return 3;
}