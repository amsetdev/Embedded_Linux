/**
 * @file rtc.c
 * @brief RTC synchronization module for the STM32MP1 A7 core.
 *
 * This module synchronizes the Linux system time with the external
 * DS3231 RTC connected to the Cortex-M4 core.
 *
 * The workflow is:
 * - Verify internet connectivity.
 * - Synchronize the Linux system clock using NTP.
 * - Read the current system date and time.
 * - Pack the date and time into the shared RETRAM format.
 * - Write the values to the M4 command registers.
 * - Wait for an acknowledgement from the M4 firmware.
 *
 * This file is intended to be used as a module. Call rtc_main()
 * from the application (for example, from a background thread).
 *
 * To monitor the RTC command registers on the A7 side:
 *
 * Hex:
 * @code
 * devmem2 0x38000014 w
 * devmem2 0x38000024 w
 * devmem2 0x38000028 w
 * @endcode
 *
 * Decimal:
 * @code
 * watch -n 1 '
 * hb=$(devmem2 0x38000014 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
 * t=$(devmem2 0x38000024 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
 * d=$(devmem2 0x38000028 w | grep -oE "0x[0-9A-Fa-f]+$" | tail -1)
 *
 * printf "HEARTBEAT : %d\n" "$hb"
 * printf "TIME      : %02d:%02d:%02d\n" \
 *     "$(( (t)>>16 & 0xFF ))" \
 *     "$(( (t)>>8  & 0xFF ))" \
 *     "$(( t & 0xFF ))"
 *
 * printf "DATE      : DOW=%d 20%02d-%02d-%02d\n" \
 *     "$(( (d)>>24 & 0xFF ))" \
 *     "$(( (d)>>16 & 0xFF ))" \
 *     "$(( (d)>>8  & 0xFF ))" \
 *     "$(( d & 0xFF ))"
 * '
 * @endcode
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

/** @brief Pointer to the mapped RETRAM memory region. */
static volatile uint8_t *retram_map = NULL;

/**
 * @brief Maps the RETRAM shared memory into the process address space.
 *
 * Opens /dev/mem and maps the RETRAM region used for communication
 * between the Cortex-A7 and Cortex-M4.
 *
 * @return 0 on success, -1 on failure.
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
 * Releases the mapped RETRAM region if it has been previously mapped.
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
 * @param val 32-bit value to write.
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
 * @brief Checks whether internet connectivity is available.
 *
 * Executes a single ICMP ping to Google's public DNS server
 * (8.8.8.8) using fork() and execlp().
 *
 * @return 1 if internet is reachable, otherwise 0.
 */
int check_internet(void)
{
    ...
}

/**
 * @brief Performs a one-time NTP synchronization.
 *
 * Executes the ntpd command to synchronize the Linux system clock.
 * Failure is ignored because another NTP service may already be
 * maintaining the system time.
 */
void sync_ntp(void)
{
    ...
}

/**
 * @brief Synchronizes the RTC through the Cortex-M4.
 *
 * The function:
 * - Checks internet connectivity.
 * - Synchronizes the Linux system clock using NTP.
 * - Reads the current local date and time.
 * - Packs the date/time into RETRAM command registers.
 * - Signals the M4 firmware.
 * - Waits for an acknowledgement.
 *
 * @return
 * - 0 : RTC updated successfully.
 * - 1 : Internet unavailable or system time error.
 * - 2 : Failed to map RETRAM.
 * - 3 : M4 acknowledgement timeout.
 */
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

    uint8_t sec = (uint8_t)tmval.tm_sec;
    uint8_t min_ = (uint8_t)tmval.tm_min;
    uint8_t hour = (uint8_t)tmval.tm_hour;
    uint8_t day = (uint8_t)tmval.tm_mday;
    uint8_t month = (uint8_t)(tmval.tm_mon + 1);
    uint8_t year = (uint8_t)(tmval.tm_year + 1900 - 2000);
    uint8_t dow = (uint8_t)(tmval.tm_wday == 0 ? 7 : tmval.tm_wday);

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
           year, month, day, hour, min_, sec, dow);

    printf("Packed: time=0x%08X date=0x%08X\n",
           time_packed, date_packed);

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