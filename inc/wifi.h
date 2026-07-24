#ifndef WIFI_H
#define WIFI_H

#include <stddef.h>

#define WIFI_CONFIG_FILE "/home/root/edb_c/linking/wifi_config.json"

typedef struct
{
    char ssid[64];
    char password[64];
} wifi_config_t;

int wifi_load_config(wifi_config_t *cfg);
int wifi_connect(void);
int wifi_is_connected(void);
int wifi_get_ip(char *ip, size_t len);

#endif