#ifndef SETTINGS_H
#define SETTINGS_H

/* ---- Defaults ---------------------------------------------------------- */
#define MODBUS_PORT_DEF  "/dev/ttySTM2"
#define MODBUS_BAUD_DEF  9600
#define MODBUS_SLAVE_DEF 1
#define MQTT_BROKER_DEF  "ee8fe21be0bd034378b2f548b0e16bce62.s1.eu.hivemq.cloud"
#define MQTT_PORT_DEF    8883
#define MQTT_USERNAME_DEF "prasad"
#define MQTT_PASSWORD_DEF "prasad#12$A"
#define INTERVAL_DEF     30

#define SETTINGS_FILE    "settings.conf"

/* ---- Settings structure ----------------------------------------------- */
typedef struct {
    char modbus_port[64];
    int  modbus_baud;
    int  modbus_slave;
    char mqtt_broker[256];
    int  mqtt_port;
    char mqtt_user[64];
    char mqtt_pass[64];
    int  interval;
} AppSettings;

extern AppSettings cfg;  /* global config instance */

/* ---- API --------------------------------------------------------------- */
void settings_defaults(void);
void settings_load(void);
void settings_save(void);

#endif /* SETTINGS_H */
