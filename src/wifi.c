/**
 * @file wifi.c
 * @brief Wi-Fi connection management.
 *
 * This module manages the Wi-Fi interface using wpa_supplicant and
 * udhcpc. It provides functions to connect to a configured wireless
 * network, monitor connection status, obtain the assigned IP address,
 * and disconnect the interface.
 */

#include "wifi.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Executes a shell command.
 *
 * @param cmd Shell command to execute.
 *
 * @return
 * - 0 if the command executed successfully.
 * - -1 if the shell could not be started or the command failed.
 */
static int run_command(const char *cmd)
{
    int ret = system(cmd);

    if (ret == -1)
    {
        perror("system");
        return -1;
    }

    return 0;
}

/**
 * @brief Connects to the configured Wi-Fi network.
 *
 * Creates a temporary wpa_supplicant configuration file using the
 * configured SSID and password, starts wpa_supplicant, obtains an
 * IP address using DHCP, and waits for the connection to complete.
 *
 * @return
 * - 0 on successful connection.
 * - -1 on failure.
 */
int wifi_connect(void)
{
    char ip[32];
    char cmd[256];

    /* Already connected */
    if (wifi_is_connected())
    {
        if (wifi_get_ip(ip, sizeof(ip)) == 0)
        {
            printf("[WiFi] Already connected (%s)\n", ip);
            return 0;
        }
    }

    /* Validate configuration */
    if (strlen(cfg.wifi_ssid) == 0)
    {
        printf("[WiFi] SSID not configured\n");
        return -1;
    }

    printf("[WiFi] Connecting to SSID: %s\n", cfg.wifi_ssid);

    FILE *fp = fopen("/tmp/wpa.conf", "w");
    if (!fp)
    {
        perror("fopen");
        return -1;
    }

    fprintf(fp,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=1\n"
            "network={\n"
            "    ssid=\"%s\"\n"
            "    psk=\"%s\"\n"
            "}\n",
            cfg.wifi_ssid,
            cfg.wifi_password);

    fclose(fp);

    /* Stop any existing Wi-Fi services */
    run_command("killall udhcpc >/dev/null 2>&1");
    run_command("killall wpa_supplicant >/dev/null 2>&1");

    sleep(1);

    if (run_command("ip link set wlan0 up") != 0)
    {
        printf("[WiFi] Failed to enable wlan0\n");
        return -1;
    }

    snprintf(cmd,
             sizeof(cmd),
             "wpa_supplicant -B -i wlan0 -c /tmp/wpa.conf");

    if (run_command(cmd) != 0)
    {
        printf("[WiFi] Failed to start wpa_supplicant\n");
        return -1;
    }

    sleep(2);

    if (run_command("udhcpc -i wlan0 >/dev/null 2>&1") != 0)
    {
        printf("[WiFi] Failed to start DHCP client\n");
        return -1;
    }

    for (int i = 0; i < 15; i++)
    {
        if (wifi_get_ip(ip, sizeof(ip)) == 0)
        {
            printf("[WiFi] Connected. IP = %s\n", ip);
            return 0;
        }

        sleep(1);
    }

    printf("[WiFi] Failed to obtain IP address\n");

    return -1;
}

/**
 * @brief Checks whether the Wi-Fi interface is connected.
 *
 * Verifies both the wireless association status and whether an IP
 * address has been assigned.
 *
 * @return
 * - 1 if connected.
 * - 0 otherwise.
 */
int wifi_is_connected(void)
{
    FILE *fp;
    char buf[128];

    fp = popen("iw dev wlan0 link", "r");
    if (!fp)
        return 0;

    int connected = 0;

    while (fgets(buf, sizeof(buf), fp))
    {
        if (strstr(buf, "Connected to"))
        {
            connected = 1;
            break;
        }
    }

    pclose(fp);

    if (!connected)
        return 0;

    return wifi_has_ip();
}

/**
 * @brief Retrieves the IPv4 address assigned to the Wi-Fi interface.
 *
 * @param ip Buffer to receive the IP address.
 * @param len Size of the destination buffer.
 *
 * @return
 * - 0 on success.
 * - -1 if no IP address is assigned.
 */
int wifi_get_ip(char *ip, size_t len)
{
    FILE *fp = popen("ip -4 addr show wlan0 | grep inet", "r");
    if (!fp)
        return -1;

    char line[256];

    if (!fgets(line, sizeof(line), fp))
    {
        pclose(fp);
        return -1;
    }

    pclose(fp);

    char *p = strstr(line, "inet ");
    if (!p)
        return -1;

    p += 5;

    char *slash = strchr(p, '/');
    if (!slash)
        return -1;

    size_t n = slash - p;

    if (n >= len)
        n = len - 1;

    memcpy(ip, p, n);
    ip[n] = '\0';

    return 0;
}

/**
 * @brief Disconnects the Wi-Fi interface.
 *
 * Stops the DHCP client, terminates wpa_supplicant, and brings the
 * wireless interface down.
 */
void wifi_disconnect(void)
{
    printf("[WiFi] Disconnecting...\n");

    run_command("killall udhcpc >/dev/null 2>&1");
    run_command("killall wpa_supplicant >/dev/null 2>&1");
    run_command("ip link set wlan0 down >/dev/null 2>&1");

    printf("[WiFi] Disconnected\n");
}

/**
 * @brief Checks whether the Wi-Fi interface has an assigned IP address.
 *
 * @return
 * - 1 if an IP address is assigned.
 * - 0 otherwise.
 */
int wifi_has_ip(void)
{
    char ip[32];

    return (wifi_get_ip(ip, sizeof(ip)) == 0);
}