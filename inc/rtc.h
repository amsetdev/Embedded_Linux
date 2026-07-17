/**
 * rtc.h
 * Header for rtc.c — A7-side RETRAM register offsets and function
 * declarations for syncing DS3231 RTC (via M4) to NTP time.
 */

#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* RETRAM physical memory map (A7 side)                                */
/* ------------------------------------------------------------------ */
#define RETRAM_PHYS_BASE     0x38000000UL
#define RETRAM_MAP_SIZE      0x1000UL

/* Command block offsets (A7 -> M4 time-set request) */
#define OFF_CMD_FLAG         0x2C
#define OFF_CMD_TIME         0x30
#define OFF_CMD_DATE         0x34
#define OFF_CMD_ACK          0x38

/* Polling configuration */
#define ACK_POLL_ATTEMPTS    5
#define ACK_POLL_DELAY_SEC   1

/* ------------------------------------------------------------------ */
/* Function declarations                                               */
/* ------------------------------------------------------------------ */

/* Maps RETRAM physical memory via /dev/mem. Returns 0 on success, -1 on failure. */
int map_retram(void);

/* Unmaps RETRAM memory previously mapped by map_retram(). */
void unmap_retram(void);

/* Writes a 32-bit value to the given RETRAM offset. */
void reg_write32(uint32_t offset, uint32_t val);

/* Reads a 32-bit value from the given RETRAM offset. */
uint32_t reg_read32(uint32_t offset);

/* Checks internet connectivity via ping. Returns 1 if reachable, 0 otherwise. */
int check_internet(void);

/* Forces a one-shot NTP sync. Best-effort; does not return status. */
void sync_ntp(void);

/* Runs the full RTC sync sequence: checks internet, syncs NTP, packs
   system time, and writes it to the M4 via RETRAM command registers.
   Returns 0 on success (M4 acked), 1 = no internet, 2 = mmap/write
   failure, 3 = M4 did not ack in time. */
int rtc_main(void);

#endif /* RTC_H */