#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "config.h"

/* ============================================================================
 * GLOBALS
 * ========================================================================== */

AppSettings       cfg;
ModbusPoint       points[MAX_POINTS];
int               point_count    = 0;
volatile int      running        = 1;
volatile int      mqtt_connected = 0;
pthread_mutex_t   points_mutex   = PTHREAD_MUTEX_INITIALIZER;
volatile int      mb_cycle       = 0;
volatile int      mb_ok_flag     = 0;
volatile int      mb_success_cnt = 0;

/* ============================================================================
 * LOGGING
 * ========================================================================== */

void log_msg(const char *level, const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", t);
    printf("[%s] [%s] ", tbuf, level);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* ============================================================================
 * SETTINGS
 * ========================================================================== */

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
}

void settings_load(void)
{
    settings_defaults();
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ') k++;
        while (*v == ' ') v++;

        if      (!strcmp(k, "modbus_port"))  strncpy(cfg.modbus_port,  v, sizeof(cfg.modbus_port)  - 1);
        else if (!strcmp(k, "modbus_baud"))  cfg.modbus_baud  = atoi(v);
        else if (!strcmp(k, "modbus_slave")) cfg.modbus_slave = atoi(v);
        else if (!strcmp(k, "mqtt_broker"))  strncpy(cfg.mqtt_broker,  v, sizeof(cfg.mqtt_broker)  - 1);
        else if (!strcmp(k, "mqtt_port"))    cfg.mqtt_port    = atoi(v);
        else if (!strcmp(k, "mqtt_user"))    strncpy(cfg.mqtt_user,    v, sizeof(cfg.mqtt_user)    - 1);
        else if (!strcmp(k, "mqtt_pass"))    strncpy(cfg.mqtt_pass,    v, sizeof(cfg.mqtt_pass)    - 1);
        else if (!strcmp(k, "interval"))     cfg.interval     = atoi(v);
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
    fclose(f);
}