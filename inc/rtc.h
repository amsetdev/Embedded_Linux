/**
 * @file rtc.h
 * @brief RTC synchronization interface.
 *
 * This module provides functions for synchronizing the DS3231 Real-Time
 * Clock (RTC) connected to the Cortex-M4 with the Linux system time
 * running on the Cortex-A7. The A7 communicates with the M4 through
 * RETRAM shared memory.
 */

#ifndef RTC_H
#define RTC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* RETRAM Memory Map                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Physical base address of RETRAM shared memory.
 */
#define RETRAM_PHYS_BASE     0x38000000UL

/**
 * @brief Size of the RETRAM memory region to map.
 */
#define RETRAM_MAP_SIZE      0x1000UL

/* -------------------------------------------------------------------------- */
/* RETRAM Command Register Offsets                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Command flag register offset.
 *
 * Writing a non-zero value notifies the M4 that a new
 * RTC update request is available.
 */
#define OFF_CMD_FLAG         0x2C

/**
 * @brief Packed time register offset.
 */
#define OFF_CMD_TIME         0x30

/**
 * @brief Packed date register offset.
 */
#define OFF_CMD_DATE         0x34

/**
 * @brief Command acknowledgment register offset.
 *
 * The M4 sets this register after successfully updating
 * the external RTC.
 */
#define OFF_CMD_ACK          0x38

/* -------------------------------------------------------------------------- */
/* Polling Configuration                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief Maximum number of acknowledgment polling attempts.
 */
#define ACK_POLL_ATTEMPTS    5

/**
 * @brief Delay between acknowledgment polling attempts (seconds).
 */
#define ACK_POLL_DELAY_SEC   1

/* -------------------------------------------------------------------------- */
/* RETRAM Access                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Maps the RETRAM shared memory into the process address space.
 *
 * Opens `/dev/mem` and maps the RETRAM region for read/write access.
 *
 * @return
 * - 0 on success.
 * - -1 on failure.
 */
int map_retram(void);

/**
 * @brief Unmaps the previously mapped RETRAM memory.
 */
void unmap_retram(void);

/**
 * @brief Writes a 32-bit value to a RETRAM register.
 *
 * @param offset Register offset from ::RETRAM_PHYS_BASE.
 * @param val Value to write.
 */
void reg_write32(uint32_t offset, uint32_t val);

/**
 * @brief Reads a 32-bit value from a RETRAM register.
 *
 * @param offset Register offset from ::RETRAM_PHYS_BASE.
 *
 * @return Register value.
 */
uint32_t reg_read32(uint32_t offset);

/* -------------------------------------------------------------------------- */
/* Network Time Synchronization                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Checks Internet connectivity.
 *
 * Sends a ping request to verify that Internet access is available.
 *
 * @return
 * - 1 if Internet connectivity is available.
 * - 0 otherwise.
 */
int check_internet(void);

/**
 * @brief Performs a one-time NTP synchronization.
 *
 * Executes a one-shot NTP update to synchronize the Linux
 * system clock with an NTP server.
 */
void sync_ntp(void);

/* -------------------------------------------------------------------------- */
/* RTC Synchronization                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Synchronizes the external RTC with the Linux system time.
 *
 * This function performs the complete RTC synchronization sequence:
 * - Verifies Internet connectivity.
 * - Synchronizes the Linux system time using NTP.
 * - Packs the current date and time.
 * - Writes the packed values to RETRAM.
 * - Signals the Cortex-M4 to update the external DS3231 RTC.
 * - Waits for acknowledgment from the M4.
 *
 * @return
 * - 0 : RTC synchronized successfully.
 * - 1 : Internet connection unavailable.
 * - 2 : Failed to access RETRAM.
 * - 3 : M4 acknowledgment timeout.
 */
int rtc_main(void);

#ifdef __cplusplus
}
#endif

#endif /* RTC_H */