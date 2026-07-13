#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_DEFAULT_PORT         502
#define MODBUS_DEFAULT_SLAVE_ID     1
#define MODBUS_START_ADDR           0
#define MODBUS_NUM_REGS             100
#define MODBUS_RESPONSE_TIMEOUT     2
#define POLL_INTERVAL_SEC           120
#define MODBUS_CONNECT_RETRY_SEC    5

#define DB_PATH                     "/tmp/modbus_data.db"

/* ─── MQTT / HiveMQ Cloud credentials ───────────────────────────── */
#define MQTT_BROKER                 "xx2sss3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT                   8883
#define MQTT_TOPIC                  "modbus/data"
#define MQTT_USERNAME               "prasad"
#define MQTT_PASSWORD               "prasad#12$A"
#define MQTT_CLIENT_ID              "stm32mp157_modbus_master"
#define MQTT_KEEPALIVE              60

typedef struct {
    char slave_ip[64];  
    int  slave_port; 
    int  slave_id;
} mb_thread_arg_t;

void *mb_thread_func1(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_MASTER_H */
