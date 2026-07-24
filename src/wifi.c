#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int json_get_string(const char *json,
                           const char *key,
                           char *value,
                           size_t value_size)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    char *p = strstr(json, pattern);
    if (!p)
        return -1;

    p = strchr(p, ':');
    if (!p)
        return -1;

    p++;

    while (*p == ' ' || *p == '\t')
        p++;

    if (*p != '"')
        return -1;

    p++;

    char *end = strchr(p, '"');
    if (!end)
        return -1;

    size_t len = end - p;

    if (len >= value_size)
        len = value_size - 1;

    memcpy(value, p, len);
    value[len] = '\0';

    return 0;
}

int wifi_load_config(wifi_config_t *cfg)
{
    FILE *fp;
    long size;
    char *json;

    if (!cfg)
        return -1;

    fp = fopen(WIFI_CONFIG_FILE, "r");
    if (!fp)
    {
        printf("[WiFi] Cannot open %s\n", WIFI_CONFIG_FILE);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);

    json = malloc(size + 1);
    if (!json)
    {
        fclose(fp);
        return -1;
    }

    fread(json, 1, size, fp);
    json[size] = '\0';

    fclose(fp);

    memset(cfg, 0, sizeof(*cfg));

    if (json_get_string(json, "ssid",
                        cfg->ssid,
                        sizeof(cfg->ssid)) != 0)
    {
        free(json);
        return -1;
    }

    if (json_get_string(json, "password",
                        cfg->password,
                        sizeof(cfg->password)) != 0)
    {
        free(json);
        return -1;
    }

    free(json);

    return 0;
}

int wifi_connect(void)
{
    //char ssid[64];
    //char password[64];
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

    /* Load configuration */
    wifi_config_t cfg;

if (wifi_load_config(&cfg) != 0)
{
    printf("[WiFi] Failed to load wifi_config.json\n");
    return -1;
}

    printf("[WiFi] Connecting to SSID: %s\n", cfg.ssid);

    /* Create WPA configuration */
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
            cfg.ssid,
            cfg.password);

    fclose(fp);

    /* Stop any old connection */
    (void)system("killall udhcpc >/dev/null 2>&1");
    (void)system("killall wpa_supplicant >/dev/null 2>&1");

    sleep(1);

    /* Bring interface up */
    (void)system("ip link set wlan0 up");

    /* Start WPA */
    snprintf(cmd,
             sizeof(cmd),
             "wpa_supplicant -B -i wlan0 -c /tmp/wpa.conf");

    if (system(cmd) != 0)
    {
        printf("[WiFi] Failed to start wpa_supplicant\n");
        return -1;
    }

    sleep(2);

    /* Get IP address */
    (void)system("udhcpc -i wlan0 >/dev/null 2>&1");

    /* Wait up to 15 seconds */
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