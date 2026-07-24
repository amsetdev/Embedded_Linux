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
    wifi_config_t cfg;

    if (wifi_load_config(&cfg) != 0)
    {
        printf("[WiFi] Failed to read config\n");
        return -1;
    }

    printf("[WiFi] SSID: %s\n", cfg.ssid);

    char cmd[512];

    snprintf(cmd, sizeof(cmd),
        "wpa_passphrase \"%s\" \"%s\" > /tmp/wpa.conf",
        cfg.ssid,
        cfg.password);
    system(cmd);

    system("pkill wpa_supplicant > /dev/null 2>&1");

    system("wpa_supplicant -B -i wlan0 -c /tmp/wpa.conf");

    sleep(3);

    system("udhcpc -i wlan0");

    sleep(2);

    return wifi_is_connected();
}

int wifi_is_connected(void)
{
    FILE *fp;
    char line[256];

    fp = popen("ip addr show wlan0", "r");
    if (!fp)
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