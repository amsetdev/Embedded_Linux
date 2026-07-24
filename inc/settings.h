#ifndef SETTINGS_H
#define SETTINGS_H

#define MODBUS_PORT_DEF  "/dev/ttyACM0"
#define MODBUS_BAUD_DEF  9600
#define MODBUS_SLAVE_DEF 1

#define MQTT_BROKER_DEF  "25d1470809e1409796c6dd8bd937c33c.s1.eu.hivemq.cloud"
#define MQTT_PORT_DEF    8883
#define MQTT_USERNAME_DEF "Aishwarya"
#define MQTT_PASSWORD_DEF "password"

#define INTERVAL_DEF     30

/* ---------------- WiFi ---------------- */

#define WIFI_ENABLE_DEF     1
#define WIFI_SSID_DEF       "Amset"
#define WIFI_PASSWORD_DEF   "12345678"
#define WIFI_COUNTRY_DEF    "IN"

/* -------------------------------------- */

#define SETTINGS_FILE    "settings.conf"

typedef struct
{
    /* Modbus */

    char modbus_port[64];
    int  modbus_baud;
    int  modbus_slave;

    /* MQTT */

    char mqtt_broker[256];
    int  mqtt_port;
    char mqtt_user[64];
    char mqtt_pass[64];

    int interval;

    /* WiFi */

    int  wifi_enable;
    char wifi_ssid[64];
    char wifi_password[64];
    char wifi_country[8];

} AppSettings;

extern AppSettings cfg;

void settings_defaults(void);
void settings_load(void);
void settings_save(void);

#endif