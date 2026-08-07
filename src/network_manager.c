/**
 * @file network_manager.c
 * @brief Network manager implementation.
 *
 * This module manages network connectivity by selecting the active
 * interface between Ethernet and Wi-Fi.
 *
 * Features:
 * - Ethernet has higher priority than Wi-Fi.
 * - Automatically switches to Wi-Fi when Ethernet is unavailable.
 * - Automatically switches back to Ethernet when it becomes available.
 * - Monitors the current network status.
 * - Provides APIs to initialize, monitor, query, and stop network
 *   connectivity.
 */

#include "network_manager.h"
#include "ethernet.h"
#include "wifi.h"
#include "settings.h"

#include <stdio.h>
#include <unistd.h>

/**
 * @brief Available network interface types.
 */
typedef enum
{
    NET_NONE = 0,      /**< No active network connection. */
    NET_ETHERNET,      /**< Ethernet is the active interface. */
    NET_WIFI           /**< Wi-Fi is the active interface. */

} network_type_t;

/**
 * @brief Currently active network interface.
 */
static network_type_t current_network = NET_NONE;

/**
 * @brief Switches the active network interface to Ethernet.
 *
 * Retrieves and displays the assigned IP address and updates the
 * current network state.
 */
static void use_ethernet(void)
{
    char ip[32];

    if (current_network == NET_ETHERNET)
        return;

    printf("\n");
    printf("=====================================\n");
    printf("[ NET ] Switching to Ethernet\n");

    if (ethernet_get_ip(ip, sizeof(ip)) == 0)
        printf("[ NET ] Ethernet IP : %s\n", ip);

    current_network = NET_ETHERNET;
}

/**
 * @brief Switches the active network interface to Wi-Fi.
 *
 * Attempts to establish a Wi-Fi connection. On success, retrieves
 * and displays the assigned IP address and updates the current
 * network state.
 */
static void use_wifi(void)
{
    char ip[32];

    if (current_network == NET_WIFI)
        return;

    printf("\n");
    printf("=====================================\n");
    printf("[ NET ] Switching to WiFi\n");


    if (wifi_is_connected())
    {
        if (wifi_get_ip(ip, sizeof(ip)) == 0)
            printf("[ NET ] WiFi IP : %s\n", ip);

        current_network = NET_WIFI;
    }
    else if (wifi_connect() == 0)
    {
        if (wifi_get_ip(ip, sizeof(ip)) == 0)
            printf("[ NET ] WiFi IP : %s\n", ip);

        current_network = NET_WIFI;
    }
    else
    {
        printf("[ NET ] WiFi connection failed\n");
        current_network = NET_NONE;
    }
}

/**
 * @brief Initializes the network manager.
 *
 * Initializes Ethernet first. If Ethernet is connected and has
 * obtained an IP address, it becomes the active interface.
 * Otherwise, Wi-Fi is used.
 */
void network_init(void)
{
    printf("\n");
    printf("=====================================\n");
    printf("[ NET ] Network Manager Start\n");

    ethernet_init();

    if (ethernet_is_connected() &&
        ethernet_has_ip())
    {
        use_ethernet();
        return;
    }

    use_wifi();
}

/**
 * @brief Monitors the network status.
 *
 * Ethernet always has priority. If Ethernet is disconnected,
 * the manager switches to Wi-Fi. If Ethernet becomes available
 * again, it automatically becomes the active interface.
 */
void network_monitor(void)
{
    /* Ethernet has highest priority */
    if (ethernet_is_connected() &&
        ethernet_has_ip())
    {
        if (current_network != NET_ETHERNET)
        {
            printf("[ NET ] Ethernet detected\n");
            use_ethernet();
        }

        return;
    }

    /* Ethernet was active but is no longer available */
    if (current_network == NET_ETHERNET)
    {
        printf("[ NET ] Ethernet disconnected\n");
        current_network = NET_NONE;
    }

    /* Wi-Fi is currently active */
    if (current_network == NET_WIFI)
    {
        if (wifi_is_connected())
            return;

        printf("[ NET ] WiFi disconnected\n");
        current_network = NET_NONE;
    }

    /* Attempt to connect using Wi-Fi */
    use_wifi();
}

/**
 * @brief Checks whether the active network is online.
 *
 * @return 1 if the active network interface is connected,
 *         otherwise 0.
 */
int network_is_online(void)
{
    switch (current_network)
    {
        case NET_ETHERNET:
            return ethernet_has_ip();

        case NET_WIFI:
            return wifi_is_connected();

        default:
            return 0;
    }
}

/**
 * @brief Stops the network manager.
 *
 * Disconnects Wi-Fi if it is currently active and clears the
 * current network state.
 */
void network_stop(void)
{
    if (current_network == NET_WIFI)
        wifi_disconnect();

    current_network = NET_NONE;

    printf("[ NET ] Network manager stopped\n");
}