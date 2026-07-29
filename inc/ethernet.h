/**
 * @file ethernet.h
 * @brief Ethernet interface management.
 *
 * This module provides helper functions for monitoring the Ethernet
 * interface, checking link status, obtaining an IP address, and
 * initializing Ethernet connectivity.
 */

#ifndef ETHERNET_H
#define ETHERNET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Ethernet network interface name.
 */
#define ETH_INTERFACE "end0"

/* -------------------------------------------------------------------------- */
/* API                                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the Ethernet interface.
 *
 * Performs any required setup for the Ethernet interface before
 * it is used by the application.
 *
 * @return
 * - 0 on success.
 * - Non-zero on failure.
 */
int ethernet_init(void);

/**
 * @brief Checks whether the Ethernet cable is connected.
 *
 * @return
 * - 1 if the Ethernet link is active.
 * - 0 otherwise.
 */
int ethernet_is_connected(void);

/**
 * @brief Checks whether the Ethernet interface has an IPv4 address.
 *
 * @return
 * - 1 if an IP address is assigned.
 * - 0 otherwise.
 */
int ethernet_has_ip(void);

/**
 * @brief Retrieves the IPv4 address assigned to the Ethernet interface.
 *
 * @param ip Buffer to receive the IP address string.
 * @param len Size of the destination buffer.
 *
 * @return
 * - 0 on success.
 * - -1 if no IP address is available.
 */
int ethernet_get_ip(char *ip, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_H */