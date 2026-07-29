/**
 * @file ethernet.c
 * @brief Ethernet interface monitoring functions.
 *
 * This module provides utility functions to monitor the Ethernet interface,
 * detect link status, check IP address availability, and retrieve the current
 * IPv4 address assigned to the interface.
 */

#include "ethernet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARRIER_FILE "/sys/class/net/end0/carrier"

/**********************************************************/

/**
 * @brief Initializes the Ethernet monitoring module.
 *
 * Prints the Ethernet interface being monitored. No hardware
 * initialization is performed.
 *
 * @return int
 * @retval 0 Initialization completed successfully.
 */
int ethernet_init(void)
{
    printf("[ ETH ] Monitoring interface %s\n", ETH_INTERFACE);
    return 0;
}

/**********************************************************/

/**
 * @brief Checks whether the Ethernet cable is connected.
 *
 * Reads the Linux carrier status file to determine whether the
 * Ethernet interface has an active physical link.
 *
 * @return int
 * @retval 1 Ethernet link is active.
 * @retval 0 Ethernet link is inactive or an error occurred.
 */
int ethernet_is_connected(void)
{
    FILE *fp = fopen("/sys/class/net/end0/carrier", "r");
    if (!fp)
        return 0;

    int carrier = 0;

    if (fscanf(fp, "%d", &carrier) != 1)
    {
        fclose(fp);
        return 0;
    }

    fclose(fp);

    return carrier;
}

/**********************************************************/

/**
 * @brief Checks whether the Ethernet interface has an IPv4 address.
 *
 * Executes the Linux "ip" command and searches for an IPv4 address
 * assigned to the configured Ethernet interface.
 *
 * @return int
 * @retval 1 IPv4 address is assigned.
 * @retval 0 No IPv4 address is assigned or an error occurred.
 */
int ethernet_has_ip(void)
{
    FILE *fp;

    char line[256];

    fp = popen("ip -4 addr show " ETH_INTERFACE, "r");

    if (fp == NULL)
        return 0;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "inet "))
        {
            pclose(fp);
            return 1;
        }
    }

    pclose(fp);

    return 0;
}

/**********************************************************/

/**
 * @brief Retrieves the current IPv4 address of the Ethernet interface.
 *
 * Executes the Linux "ip" command, extracts the assigned IPv4 address,
 * and stores it in the user-provided buffer.
 *
 * @param[out] ip Buffer to store the IPv4 address string.
 * @param[in] len Size of the destination buffer.
 *
 * @return int
 * @retval 0 IPv4 address retrieved successfully.
 * @retval -1 Failed to retrieve an IPv4 address.
 */
int ethernet_get_ip(char *ip, size_t len)
{
    FILE *fp;

    char line[256];

    fp = popen("ip -4 addr show " ETH_INTERFACE, "r");

    if (fp == NULL)
        return -1;

    while (fgets(line, sizeof(line), fp))
    {
        char *p = strstr(line, "inet ");

        if (p)
        {
            sscanf(p, "inet %63[^/]", ip);

            pclose(fp);

            return 0;
        }
    }

    pclose(fp);

    return -1;
}