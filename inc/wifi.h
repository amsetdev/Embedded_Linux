/**
 * @file wifi.h
 * @brief Network status and Wi-Fi configuration management.
 *
 * Linux/systemd manages the actual network interfaces, DHCP, routing,
 * and Wi-Fi connection lifecycle.
 *
 * This module:
 * - Reads Wi-Fi/Ethernet status.
 * - Retrieves IPv4 addresses.
 * - Applies Wi-Fi credentials from smart_rtu_config.json.
 * - Reconfigures the existing wpa_supplicant service.
 * - Waits for Wi-Fi association and IPv4 address.
 *
 * The application does NOT start or stop the Wi-Fi service.
 */

#ifndef WIFI_H
#define WIFI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Wi-Fi status                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Checks whether wlan0 is associated with an access point.
 *
 * Uses the Linux wireless status to determine whether wlan0 is
 * associated with a Wi-Fi access point.
 *
 * @return
 * - 1 if associated.
 * - 0 if not associated.
 */
int wifi_is_connected(void);

/**
 * @brief Checks whether wlan0 is associated and has an IPv4 address.
 *
 * @return
 * - 1 if Wi-Fi is fully online.
 * - 0 otherwise.
 */
int wifi_is_online(void);

/**
 * @brief Checks whether wlan0 has an IPv4 address.
 *
 * @return
 * - 1 if an IPv4 address exists.
 * - 0 otherwise.
 */
int wifi_has_ip(void);

/**
 * @brief Gets the IPv4 address assigned to wlan0.
 *
 * @param[out] ip Destination buffer.
 * @param[in]  len Size of destination buffer.
 *
 * @return
 * - 0 on success.
 * - -1 on failure.
 */
int wifi_get_ip(char *ip, size_t len);

/**
 * @brief Waits for Wi-Fi to become fully operational.
 *
 * Repeatedly checks whether wlan0 is associated and has an IPv4
 * address until the connection succeeds or the timeout expires.
 *
 * This function does not start or stop wpa_supplicant.
 *
 * @param timeout_seconds Maximum time to wait in seconds.
 *
 * @return
 * - 0 if Wi-Fi becomes ready.
 * - -1 if the timeout expires.
 */
int wifi_wait_for_connection(int timeout_seconds);

/* -------------------------------------------------------------------------- */
/* Ethernet status                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Checks whether Ethernet physical link is active.
 *
 * Reads the carrier state of end0.
 *
 * @return
 * - 1 if Ethernet link is active.
 * - 0 otherwise.
 */
int ethernet_is_connected(void);

/**
 * @brief Checks whether Ethernet is fully online.
 *
 * Ethernet is considered online when it has an IPv4 address.
 *
 * @return
 * - 1 if Ethernet has an IPv4 address.
 * - 0 otherwise.
 */
int ethernet_is_online(void);

/**
 * @brief Checks whether Ethernet has an IPv4 address.
 *
 * @return
 * - 1 if an IPv4 address exists.
 * - 0 otherwise.
 */
int ethernet_has_ip(void);

/**
 * @brief Gets the IPv4 address assigned to end0.
 *
 * @param[out] ip Destination buffer.
 * @param[in]  len Size of destination buffer.
 *
 * @return
 * - 0 on success.
 * - -1 on failure.
 */
int ethernet_get_ip(char *ip, size_t len);

/* -------------------------------------------------------------------------- */
/* General network status                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief Checks whether any network interface is online.
 *
 * Linux routing determines which interface is actually used.
 *
 * This function only reports whether Ethernet or Wi-Fi has
 * an IPv4 address.
 *
 * @return
 * - 1 if Ethernet or Wi-Fi is online.
 * - 0 if neither is online.
 */
int network_is_online(void);

/**
 * @brief Prints current Ethernet and Wi-Fi status.
 *
 * Example output:
 *
 *     ========== NETWORK STATUS ==========
 *     Ethernet : DISCONNECTED
 *     WiFi     : CONNECTED
 *     WiFi IP  : 192.168.137.45
 *     ====================================
 */
void network_print_status(void);

/* -------------------------------------------------------------------------- */
/* Wi-Fi configuration                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Applies Wi-Fi settings from the global cfg structure.
 *
 * The Wi-Fi SSID, password and country are read from:
 *
 *     smart_rtu_config.json
 *
 * through settings.c.
 *
 * This function updates:
 *
 *     /etc/wpa_supplicant/wpa_supplicant-wlan0.conf
 *
 * and asks the already-running:
 *
 *     wpa_supplicant@wlan0.service
 *
 * to reload its configuration.
 *
 * IMPORTANT:
 * This function does NOT:
 *
 * - kill wpa_supplicant
 * - start another wpa_supplicant process
 * - kill udhcpc
 * - bring wlan0 down
 *
 * Linux/systemd remains responsible for the Wi-Fi service lifecycle.
 *
 * @return
 * - 0 on successful configuration/reconfigure request.
 * - -1 on failure.
 */
int wifi_reconfigure(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_H */