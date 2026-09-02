/**
 * @file wifi.c
 * @brief Network status and Wi-Fi configuration management.
 *
 * Linux/systemd owns the network interfaces, DHCP, routing,
 * and wpa_supplicant service.
 *
 * This module only:
 *
 * 1. Reads Wi-Fi status.
 * 2. Reads Ethernet status.
 * 3. Retrieves interface IP addresses.
 * 4. Applies Wi-Fi settings from cfg.
 * 5. Reloads the existing wpa_supplicant service.
 */

#include "wifi.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/* -------------------------------------------------------------------------- */
/* Interface configuration                                                    */
/* -------------------------------------------------------------------------- */

#define WIFI_INTERFACE      "wlan0"
#define ETH_INTERFACE       "end0"

#define WIFI_CONFIG_FILE \
    "/etc/wpa_supplicant/wpa_supplicant-wlan0.conf"

/* -------------------------------------------------------------------------- */
/* Internal helper                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Executes a shell command and checks its exit status.
 *
 * @param cmd Command to execute.
 *
 * @return 0 on success, -1 on failure.
 */
static int run_command(const char *cmd)
{
    int rc;

    if (cmd == NULL)
        return -1;

    rc = system(cmd);

    if (rc == -1)
        return -1;

    if (!WIFEXITED(rc))
        return -1;

    if (WEXITSTATUS(rc) != 0)
        return -1;

    return 0;
}

/**
 * @brief Gets the IPv4 address of an interface.
 *
 * @param interface Interface name.
 * @param ip Destination buffer.
 * @param len Destination buffer size.
 *
 * @return 0 on success, -1 on failure.
 */
static int get_interface_ip(const char *interface,
                            char *ip,
                            size_t len)
{
    char command[128];
    char line[256];

    if (interface == NULL || ip == NULL || len == 0)
        return -1;

    ip[0] = '\0';

    snprintf(command,
             sizeof(command),
             "ip -4 addr show %s 2>/dev/null | grep 'inet '",
             interface);

    FILE *fp = popen(command, "r");

    if (fp == NULL)
        return -1;

    if (fgets(line, sizeof(line), fp) == NULL)
    {
        pclose(fp);
        return -1;
    }

    pclose(fp);

    char *p = strstr(line, "inet ");

    if (p == NULL)
        return -1;

    p += 5;

    char *slash = strchr(p, '/');

    if (slash == NULL)
        return -1;

    size_t n = (size_t)(slash - p);

    if (n >= len)
        n = len - 1;

    memcpy(ip, p, n);

    ip[n] = '\0';

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Wi-Fi status                                                               */
/* -------------------------------------------------------------------------- */

int wifi_is_connected(void)
{
    FILE *fp;
    char line[256];

    fp = popen(
        "iw dev " WIFI_INTERFACE " link 2>/dev/null",
        "r");

    if (fp == NULL)
        return 0;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "Connected to") != NULL)
        {
            pclose(fp);
            return 1;
        }
    }

    pclose(fp);

    return 0;
}

int wifi_get_ip(char *ip, size_t len)
{
    return get_interface_ip(WIFI_INTERFACE, ip, len);
}

int wifi_has_ip(void)
{
    char ip[32];

    return wifi_get_ip(ip, sizeof(ip)) == 0;
}

int wifi_is_online(void)
{
    if (!wifi_is_connected())
        return 0;

    if (!wifi_has_ip())
        return 0;

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Ethernet status                                                            */
/* -------------------------------------------------------------------------- */

int ethernet_is_connected(void)
{
    FILE *fp;
    int carrier = 0;

    fp = fopen(
        "/sys/class/net/" ETH_INTERFACE "/carrier",
        "r");

    if (fp == NULL)
        return 0;

    if (fscanf(fp, "%d", &carrier) != 1)
        carrier = 0;

    fclose(fp);

    return carrier == 1;
}

int ethernet_get_ip(char *ip, size_t len)
{
    return get_interface_ip(ETH_INTERFACE, ip, len);
}

int ethernet_has_ip(void)
{
    char ip[32];

    return ethernet_get_ip(ip, sizeof(ip)) == 0;
}

int ethernet_is_online(void)
{
    if (!ethernet_is_connected())
        return 0;

    if (!ethernet_has_ip())
        return 0;

    return 1;
}

/* -------------------------------------------------------------------------- */
/* General network status                                                     */
/* -------------------------------------------------------------------------- */

int network_is_online(void)
{
    /*
     * Linux routing decides which interface is actually used.
     *
     * We only report whether at least one interface is usable.
     */

    if (ethernet_is_online())
        return 1;

    if (wifi_is_online())
        return 1;

    return 0;
}

void network_print_status(void)
{
    char ip[32];

    printf("\n========== NETWORK STATUS ==========\n");

    /* Ethernet */

    if (ethernet_is_online())
    {
        if (ethernet_get_ip(ip, sizeof(ip)) == 0)
            printf("Ethernet : CONNECTED  IP=%s\n", ip);
        else
            printf("Ethernet : CONNECTED\n");
    }
    else if (ethernet_is_connected())
    {
        printf("Ethernet : LINK UP, NO IP\n");
    }
    else
    {
        printf("Ethernet : DISCONNECTED\n");
    }

    /* Wi-Fi */

    if (wifi_is_online())
    {
        if (wifi_get_ip(ip, sizeof(ip)) == 0)
            printf("WiFi     : CONNECTED  IP=%s\n", ip);
        else
            printf("WiFi     : CONNECTED\n");
    }
    else if (wifi_is_connected())
    {
        printf("WiFi     : ASSOCIATED, NO IP\n");
    }
    else
    {
        printf("WiFi     : DISCONNECTED\n");
    }

    printf("====================================\n");
}

/* -------------------------------------------------------------------------- */
/* Wi-Fi configuration                                                        */
/* -------------------------------------------------------------------------- */

int wifi_reconfigure(void)
{
    FILE *fp;

    /*
     * Validate configuration loaded by settings.c.
     */

    if (cfg.wifi_enable == 0)
    {
        printf("[WiFi] Wi-Fi disabled in configuration\n");
        return 0;
    }

    if (cfg.wifi_ssid[0] == '\0')
    {
        printf("[WiFi] SSID is empty\n");
        return -1;
    }

    if (cfg.wifi_password[0] == '\0')
    {
        printf("[WiFi] Wi-Fi password is empty\n");
        return -1;
    }

    printf("[WiFi] Applying configuration for SSID: %s\n",
           cfg.wifi_ssid);

    /*
     * Write the configuration used by the systemd
     * wpa_supplicant@wlan0.service.
     */

    fp = fopen(WIFI_CONFIG_FILE, "w");

    if (fp == NULL)
    {
        perror("[WiFi] Cannot open WPA configuration");
        return -1;
    }

    fprintf(fp,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=1\n"
            "country=%s\n"
            "ap_scan=1\n"
            "fast_reauth=1\n"
            "\n"
            "network={\n"
            "    ssid=\"%s\"\n"
            "    psk=\"%s\"\n"
            "}\n",
            cfg.wifi_country,
            cfg.wifi_ssid,
            cfg.wifi_password);

    if (fclose(fp) != 0)
    {
        perror("[WiFi] Failed to close WPA configuration");
        return -1;
    }


    if (run_command(
            "wpa_cli -i wlan0 reconfigure >/dev/null 2>&1") != 0)
    {
        printf("[WiFi] wpa_cli reconfigure failed\n");
        return -1;
    }

    printf("[WiFi] Configuration applied successfully\n");

    return 0;
}


int wifi_wait_for_connection(int timeout_seconds)
{
    char ip[32];

    for (int elapsed = 0; elapsed < timeout_seconds; elapsed++)
    {
        if (wifi_is_connected() &&
            wifi_get_ip(ip, sizeof(ip)) == 0)
        {
            printf("[WiFi] Connected. IP = %s\n", ip);
            return 0;
        }

        sleep(1);
    }

    printf("[WiFi] Connection timeout after %d seconds\n",
           timeout_seconds);

    return -1;
}