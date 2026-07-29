/**
 * @file wifi.h
 * @brief Wi-Fi network management interface.
 *
 * This module provides functions to connect to a Wi-Fi network,
 * check connection status, retrieve the assigned IP address,
 * and disconnect from the network.
 */

#ifndef WIFI_H
#define WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "settings.h"

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Connects to the configured Wi-Fi network.
 *
 * Uses the Wi-Fi SSID and password from the application configuration
 * to establish a wireless connection and obtain an IP address.
 *
 * @return
 * - 0 on successful connection.
 * - -1 if the connection fails.
 */
int wifi_connect(void);

/**
 * @brief Checks whether Wi-Fi is connected.
 *
 * Verifies that the wireless interface is associated with an
 * access point and has a valid IP address.
 *
 * @return
 * - 1 if connected.
 * - 0 otherwise.
 */
int wifi_is_connected(void);

/**
 * @brief Checks whether the Wi-Fi interface has an IP address.
 *
 * @return
 * - 1 if an IP address is assigned.
 * - 0 otherwise.
 */
int wifi_has_ip(void);

/**
 * @brief Retrieves the current Wi-Fi IP address.
 *
 * @param ip Buffer to receive the IP address string.
 * @param len Size of the output buffer.
 *
 * @return
 * - 0 on success.
 * - -1 if no IP address is available.
 */
int wifi_get_ip(char *ip, size_t len);

/**
 * @brief Disconnects the Wi-Fi interface.
 *
 * Stops the DHCP client, terminates the WPA supplicant process,
 * and brings the wireless interface down.
 */
void wifi_disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */