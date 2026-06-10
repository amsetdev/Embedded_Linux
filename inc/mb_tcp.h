#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Configuration ──────────────────────────────────────────────── */
#define MODBUS_DEFAULT_PORT         502
#define MODBUS_DEFAULT_SLAVE_ID     1
#define MODBUS_START_ADDR           0
#define MODBUS_NUM_REGS             100
#define MODBUS_RESPONSE_TIMEOUT     2
#define POLL_INTERVAL_SEC           120

/* SQLite DB path on the target filesystem */
#define DB_PATH                     "/tmp/modbus_data.db"

/* ─── MQTT / HiveMQ Cloud credentials ───────────────────────────── */
#define MQTT_BROKER                 "xx2sss3040e50ebdbb4f949b7ec9480b0a0326.s1.eu.hivemq.cloud"
#define MQTT_PORT                   8883
#define MQTT_TOPIC                  "modbus/data"
#define MQTT_USERNAME               "prasad"
#define MQTT_PASSWORD               "prasad#12$A"
#define MQTT_CLIENT_ID              "stm32mp157_modbus_master"
#define MQTT_KEEPALIVE              60

/* ─── Thread argument ────────────────────────────────────────────── */
/**
 * Pass a pointer to this struct as the arg to pthread_create().
 * All fields are read-only after the thread starts.
 *
 * Example (static config):
 *
 *   mb_thread_arg_t mb_arg = {
 *       .slave_ip   = "192.168.1.10",
 *       .slave_port = 502,
 *       .slave_id   = 1,
 *   };
 *   pthread_create(&mb_thread_id, NULL, mb_thread_func, &mb_arg);
 *
 * slave_port / slave_id are optional – pass 0 to use the defaults.
 */
typedef struct {
    char slave_ip[64];   /**< IP address of the Modbus TCP slave (required) */
    int  slave_port;     /**< TCP port  – 0 → MODBUS_DEFAULT_PORT           */
    int  slave_id;       /**< Modbus unit id – 0 → MODBUS_DEFAULT_SLAVE_ID  */
} mb_thread_arg_t;

/* ─── Public API ─────────────────────────────────────────────────── */
/**
 * mb_thread_func() – Modbus polling thread entry point.
 *
 * Connects to the Modbus TCP slave, reads MODBUS_NUM_REGS holding
 * registers every POLL_INTERVAL_SEC seconds, stores them in SQLite,
 * and publishes them to HiveMQ Cloud over MQTT/TLS.
 *
 * The thread runs indefinitely; it only exits if reconnection fails.
 *
 * @param arg  Pointer to a mb_thread_arg_t (must remain valid for the
 *             lifetime of the thread).
 * @return     NULL always.
 *
 * Usage:
 *   pthread_t mb_thread_id;
 *   mb_thread_arg_t mb_arg = { .slave_ip = "192.168.1.10" };
 *   pthread_create(&mb_thread_id, NULL, mb_thread_func, &mb_arg);
 *   printf("[MB] Background thread started\n");
 */
void *mb_thread_func1(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_MASTER_H */