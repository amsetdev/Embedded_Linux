/**
 * @file watchdog.h
 * @brief Hardware watchdog and thread health monitoring interface.
 *
 * Provides three layers of health monitoring:
 * - Hardware watchdog via /dev/watchdog (Linux watchdog driver).
 * - Thread heartbeat registry with configurable staleness threshold.
 * - Monitor thread that pets the hardware watchdog only when all
 *   registered threads are healthy.
 *
 * If any thread stops reporting heartbeats beyond the configured
 * timeout, the monitor stops petting the hardware watchdog and
 * the board reboots automatically.
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief Maximum number of threads that can be monitored. */
#define WDG_MAX_THREADS     8

/** @brief Monitor thread check interval in seconds. */
#define WDG_CHECK_INTERVAL  10

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the watchdog subsystem.
 *
 * Opens /dev/watchdog and prepares the heartbeat registry.
 * If the hardware watchdog cannot be opened, monitoring still
 * works in software-only mode (logs stale threads but cannot
 * force a reboot).
 *
 * @return 1 on success, 0 on failure.
 */
int watchdog_init(void);

/**
 * @brief Registers a thread for health monitoring.
 *
 * Must be called before the thread starts sending heartbeats.
 * Thread-safe.
 *
 * @param name  Human-readable thread name (e.g., "modbus_rtu").
 *
 * @return Non-negative thread ID on success, -1 if registry is full.
 */
int watchdog_register(const char *name);

/**
 * @brief Reports a heartbeat from a monitored thread.
 *
 * Should be called once per iteration of the thread's main loop.
 * Thread-safe.
 *
 * @param id  Thread ID returned by watchdog_register().
 */
void watchdog_heartbeat(int id);

/**
 * @brief Watchdog monitor thread function.
 *
 * Checks all registered threads every WDG_CHECK_INTERVAL seconds.
 * Pets the hardware watchdog only when all threads are healthy.
 *
 * @param arg  Unused.
 * @return Always NULL.
 */
void *watchdog_thread_func(void *arg);

/**
 * @brief Cleans up the watchdog subsystem.
 *
 * Writes the magic close character to /dev/watchdog to disable
 * it on graceful shutdown, then closes the file descriptor.
 */
void watchdog_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_H */
