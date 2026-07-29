/**
 * @file network_manager.h
 * @brief Network management interface.
 *
 * This module manages the available network interfaces and automatically
 * switches between Ethernet and Wi-Fi. Ethernet has higher priority and
 * is used whenever it is available. Wi-Fi is used as a fallback when
 * Ethernet is disconnected.
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the network manager.
 *
 * Initializes the available network interfaces and selects the
 * preferred connection. Ethernet is selected if available;
 * otherwise, the manager attempts to connect to Wi-Fi.
 */
void network_init(void);

/**
 * @brief Monitors the network status.
 *
 * Checks the status of the Ethernet and Wi-Fi interfaces and
 * automatically switches between them when necessary.
 *
 * This function should be called periodically by the application.
 */
void network_monitor(void);

/**
 * @brief Checks whether the system currently has network connectivity.
 *
 * @return
 * - 1 if the active network interface is connected.
 * - 0 if no network connection is available.
 */
int network_is_online(void);

/**
 * @brief Stops the network manager.
 *
 * Disconnects the active Wi-Fi connection if necessary and
 * releases network-related resources.
 */
void network_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_MANAGER_H */