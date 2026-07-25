#include "wifi.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>



int wifi_connect(void)
{
    char ip[32];
    char cmd[256];

    /* Already connected? */
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

    /* Stop previous WiFi connection */
    system("killall udhcpc >/dev/null 2>&1");
    system("killall wpa_supplicant >/dev/null 2>&1");

    sleep(1);

    system("ip link set wlan0 up");

    snprintf(cmd,
             sizeof(cmd),
             "wpa_supplicant -B -i wlan0 -c /tmp/wpa.conf");

    if (system(cmd) != 0)
    {
        printf("[WiFi] Failed to start wpa_supplicant\n");
        return -1;
    }

    sleep(2);

    system("udhcpc -i wlan0 >/dev/null 2>&1");

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

int wifi_is_connected(void)
{
    FILE *fp;
    char buf[128];

    /* Check association */
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

    /* Check IP address */
    return wifi_has_ip();
}

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

void wifi_disconnect(void)
{
    printf("[WIFI] Disconnecting...\n");

    /* Release DHCP lease (ignore if not running) */
    system("killall udhcpc >/dev/null 2>&1");

    /* Stop WPA Supplicant */
    system("killall wpa_supplicant >/dev/null 2>&1");

    /* Bring interface down */
    system("ip link set wlan0 down >/dev/null 2>&1");

    printf("[WIFI] Disconnected\n");
}

int wifi_has_ip(void)
{
    char ip[32];

    if (wifi_get_ip(ip, sizeof(ip)) == 0)
        return 1;

    return 0;
}