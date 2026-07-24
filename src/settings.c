#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

AppSettings cfg;  /* global config instance */

void settings_defaults(void)
{
    strncpy(cfg.modbus_port, MODBUS_PORT_DEF, sizeof(cfg.modbus_port) - 1);
    cfg.modbus_baud  = MODBUS_BAUD_DEF;
    cfg.modbus_slave = MODBUS_SLAVE_DEF;
    strncpy(cfg.mqtt_broker, MQTT_BROKER_DEF, sizeof(cfg.mqtt_broker) - 1);
    cfg.mqtt_port = MQTT_PORT_DEF;
    strncpy(cfg.mqtt_user, MQTT_USERNAME_DEF, sizeof(cfg.mqtt_user) - 1);
    strncpy(cfg.mqtt_pass, MQTT_PASSWORD_DEF, sizeof(cfg.mqtt_pass) - 1);
    cfg.interval = INTERVAL_DEF;

    cfg.wifi_enable = WIFI_ENABLE_DEF;

    strncpy(cfg.wifi_ssid,
            WIFI_SSID_DEF,
            sizeof(cfg.wifi_ssid) - 1);

    strncpy(cfg.wifi_password,
            WIFI_PASSWORD_DEF,
            sizeof(cfg.wifi_password) - 1);

    strncpy(cfg.wifi_country,
            WIFI_COUNTRY_DEF,
            sizeof(cfg.wifi_country) - 1);
}

void settings_load(void)
{
    settings_defaults();
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f)
        return;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ')
            k++;
        while (*v == ' ')
            v++;

        if (!strcmp(k, "modbus_port"))
            strncpy(cfg.modbus_port, v, sizeof(cfg.modbus_port) - 1);
        else if (!strcmp(k, "modbus_baud"))
            cfg.modbus_baud = atoi(v);
        else if (!strcmp(k, "modbus_slave"))
            cfg.modbus_slave = atoi(v);
        else if (!strcmp(k, "mqtt_broker"))
            strncpy(cfg.mqtt_broker, v, sizeof(cfg.mqtt_broker) - 1);
        else if (!strcmp(k, "mqtt_port"))
            cfg.mqtt_port = atoi(v);
        else if (!strcmp(k, "mqtt_user"))
            strncpy(cfg.mqtt_user, v, sizeof(cfg.mqtt_user) - 1);
        else if (!strcmp(k, "mqtt_pass"))
            strncpy(cfg.mqtt_pass, v, sizeof(cfg.mqtt_pass) - 1);
        else if (!strcmp(k, "interval"))
            cfg.interval = atoi(v);
        else if (!strcmp(k, "wifi_enable"))
            cfg.wifi_enable = atoi(v);

        else if (!strcmp(k, "wifi_ssid"))
            strncpy(cfg.wifi_ssid, v, sizeof(cfg.wifi_ssid) - 1);

        else if (!strcmp(k, "wifi_password"))
            strncpy(cfg.wifi_password, v, sizeof(cfg.wifi_password) - 1);

        else if (!strcmp(k, "wifi_country"))
            strncpy(cfg.wifi_country, v, sizeof(cfg.wifi_country) - 1);
    }
    fclose(f);
}

void settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "modbus_port=%s\n",  cfg.modbus_port);
    fprintf(f, "modbus_baud=%d\n",  cfg.modbus_baud);
    fprintf(f, "modbus_slave=%d\n", cfg.modbus_slave);
    fprintf(f, "mqtt_broker=%s\n",  cfg.mqtt_broker);
    fprintf(f, "mqtt_port=%d\n",    cfg.mqtt_port);
    fprintf(f, "mqtt_user=%s\n",    cfg.mqtt_user);
    fprintf(f, "mqtt_pass=%s\n",    cfg.mqtt_pass);
    fprintf(f, "interval=%d\n",     cfg.interval);
    fprintf(f,"wifi_enable=%d\n",cfg.wifi_enable);
    fprintf(f, "wifi_ssid=%s\n", cfg.wifi_ssid);
    fprintf(f, "wifi_password=%s\n", cfg.wifi_password);
    fprintf(f, "wifi_country=%s\n", cfg.wifi_country);
    fclose(f);
}
