/**
 * @file connection.h
 * @brief Internet connectivity monitoring interface.
 *
 * This module provides functions to start and stop a background
 * thread that periodically checks internet connectivity. The
 * current connection status is available through the
 * ::internet_up flag.
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Indicates whether internet connectivity is available.
 *
 * Value meanings:
 * - 1 : Internet connection is available.
 * - 0 : Internet connection is unavailable.
 */
extern atomic_int internet_up;

/**
 * @brief Starts the internet connectivity monitoring thread.
 *
 * Creates a background thread that periodically checks whether
 * the device has internet access and updates ::internet_up.
 */
void connection_init(void);

/**
 * @brief Stops the internet connectivity monitoring thread.
 *
 * Signals the background monitoring thread to terminate and
 * waits for it to exit cleanly.
 */
void connection_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* CONNECTION_H */