#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <pthread.h>
#include <sqlite3.h>
#include <mosquitto.h>

/* ============================================================================
 * CONFIGURATION — edit or override via settings.conf at runtime
 * ========================================================================== */

#define MODBUS_PORT_DEF      "/dev/ttyACM0"
#define MODBUS_BAUD_DEF      9600
#define MODBUS_SLAVE_DEF     1
#define MODBUS_PARITY        'N'
#define MODBUS_DATA_BITS     8
#define MODBUS_STOP_BITS     1

#define CONFIG_FILE          "registers.csv"
#define SETTINGS_FILE        "settings.conf"

#define MQTT_BROKER_DEF      "3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT_DEF        8883
#define MQTT_TOPIC           "modbus/data"
#define MQTT_USERNAME_DEF    "prasad"
#define MQTT_PASSWORD_DEF    "prasad#12$A"
#define MQTT_STORAGE_DB      "mqtt_storage.db"

#define INTERVAL_DEF         30
#define MAX_RETRIES          2
#define POINT_DELAY_US       10000
#define MAX_POINTS           2000
#define LABEL_MAX            64
#define UNIT_MAX             16
#define PAYLOAD_MAX          131072

/* Display */
#define DRM_DEVICE           "/dev/dri/card0"
#define DISP_W               480
#define DISP_H               800

/* ============================================================================
 * SETTINGS
 * ========================================================================== */

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

extern AppSettings cfg;

void settings_defaults(void);
void settings_load(void);
void settings_save(void);

/* ============================================================================
 * DATA STRUCTURES
 * ========================================================================== */

typedef enum { REG_HOLDING, REG_INPUT, REG_COIL, REG_DISCRETE } RegType;

typedef struct {
    char    label[LABEL_MAX];
    int     address;
    RegType reg_type;
    char    data_type;
    char    unit[UNIT_MAX];
    int     value;
    int     valid;
} ModbusPoint;

/* ============================================================================
 * GLOBALS (defined in main.c)
 * ========================================================================== */

extern ModbusPoint        points[MAX_POINTS];
extern int                point_count;
extern volatile int       running;
extern volatile int       mqtt_connected;
extern pthread_mutex_t    points_mutex;
extern volatile int       mb_cycle;
extern volatile int       mb_ok_flag;
extern volatile int       mb_success_cnt;

/* ============================================================================
 * LOGGING
 * ========================================================================== */

void log_msg(const char *level, const char *fmt, ...);

#define LOG_INFO(...)  log_msg("INFO",    __VA_ARGS__)
#define LOG_WARN(...)  log_msg("WARNING", __VA_ARGS__)
#define LOG_ERROR(...) log_msg("ERROR",   __VA_ARGS__)

#endif /* CONFIG_H */