/**
 * @file mqtt.c
 * @brief MQTT client implementation for AWS IoT Core.
 *
 * Handles MQTT client initialization with X.509 mutual TLS,
 * JSON payload construction from Modbus register data,
 * message publishing with offline storage fallback,
 * and broker reconnection logic.
 */

#include "mqtt.h"
#include "payload.h"
#include "settings.h"
#include "data.h"
#include "storage.h"
#include "connection.h"
#include "ota.h"
#include "config_push.h"
#include "mb_cmd.h"
#include "json.h"
#include "paths.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

#include <mosquitto.h>

/** @brief Indicates whether the MQTT client is currently connected. */
atomic_int mqtt_connected = 0;

/** @brief Mosquitto client instance. */
static struct mosquitto *mosq = NULL;

/** @brief Number of recent PUBACKed message IDs remembered for mqtt_publish_confirmed(). */
#define ACKED_RING 32

/** @brief Protects acked_mids / acked_next. */
static pthread_mutex_t pub_mutex = PTHREAD_MUTEX_INITIALIZER;

/** @brief Signalled whenever the broker confirms a publish. */
static pthread_cond_t pub_cond = PTHREAD_COND_INITIALIZER;

/** @brief Message IDs of the last ACKED_RING confirmed publishes (0 = empty). */
static int acked_mids[ACKED_RING];

/** @brief Next slot to write in acked_mids. */
static unsigned acked_next = 0;

/**
 * @brief Publish callback: the broker confirmed message @p mid (PUBACK for QoS 1).
 *
 * @param m   Mosquitto instance.
 * @param ud  User data (unused).
 * @param mid Message ID.
 */
static void on_publish(struct mosquitto *m, void *ud, int mid)
{
    (void)m;
    (void)ud;
    pthread_mutex_lock(&pub_mutex);
    acked_mids[acked_next++ % ACKED_RING] = mid;
    pthread_cond_broadcast(&pub_cond);
    pthread_mutex_unlock(&pub_mutex);
}

/**
 * @brief 1 if @p mid is among the recently confirmed message IDs. Call with pub_mutex held.
 *
 * @param mid Message ID.
 * @return 1 or 0.
 */
static int mid_acked(int mid)
{
    for (int i = 0; i < ACKED_RING; i++)
        if (acked_mids[i] == mid)
            return 1;
    return 0;
}

/* Documented in mqtt.h. */
int mqtt_publish_confirmed(const char *payload, int timeout_ms)
{
    if (!mosq || !payload || !atomic_load(&mqtt_connected))
        return -1;

    int mid = 0;

    if (mosquitto_publish(mosq, &mid, cfg.mqtt_topic, (int)strlen(payload),
                          payload, 1, false) != MOSQ_ERR_SUCCESS || mid == 0)
        return -1;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&pub_mutex);
    int acked = mid_acked(mid);
    while (!acked)
    {
        if (pthread_cond_timedwait(&pub_cond, &pub_mutex, &deadline) == ETIMEDOUT)
            break;
        acked = mid_acked(mid);
    }
    acked = mid_acked(mid);
    pthread_mutex_unlock(&pub_mutex);

    return acked ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* Configuration pushed over MQTT (config_push.h)                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief cp_ops_t.read_current: the configuration file in use.
 * @param ctx Unused.
 * @return malloc()ed content, or NULL if there is none.
 */
static char *cp_read_current(void *ctx)
{
    (void)ctx;
    return read_file(paths_config());
}

/**
 * @brief cp_ops_t.write_atomic: write \<file\>.tmp, flush it to disk, rename over the file.
 * @param ctx Unused.
 * @param content New configuration.
 * @param len Its length.
 * @return 0 on success, -1 on failure (the old file is untouched).
 */
static int cp_write_atomic(void *ctx, const char *content, size_t len)
{
    (void)ctx;
    char tmp[PATHS_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", paths_config());

    FILE *f = fopen(tmp, "w");
    if (!f)
    {
        fprintf(stderr, "[MQTT] Config push: fopen %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    int ok = fwrite(content, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;

    if (fclose(f) != 0 || !ok || rename(tmp, paths_config()) != 0)
    {
        fprintf(stderr, "[MQTT] Config push: writing %s failed: %s\n", paths_config(), strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

/**
 * @brief cp_ops_t.request_reload: settings_request_reload().
 * @param ctx Unused.
 */
static void cp_request_reload(void *ctx)
{
    (void)ctx;
    settings_request_reload();
}

/** @brief The real side effects of a configuration push. */
static const cp_ops_t cp_ops = {
    .ctx            = NULL,
    .read_current   = cp_read_current,
    .write_atomic   = cp_write_atomic,
    .request_reload = cp_request_reload,
};

/**
 * @brief Applies a configuration pushed on the config topic and publishes the ack.
 *
 * @param m   Mosquitto instance.
 * @param msg The message.
 */
static void handle_config_push(struct mosquitto *m, const struct mosquitto_message *msg)
{
    char ack[128];
    char ack_topic[160];

    cp_result_t r = config_push_apply(&cp_ops, (const char *)msg->payload,
                                      msg->payloadlen > 0 ? (size_t)msg->payloadlen : 0,
                                      ack, sizeof(ack));

    printf("[MQTT] Config push: %s\n", ack);

    snprintf(ack_topic, sizeof(ack_topic), CONFIG_ACK_TOPIC_FMT, cfg.device_id);
    mosquitto_publish(m, NULL, ack_topic, (int)strlen(ack), ack, 1, false);
    (void)r;
}

/* -------------------------------------------------------------------------- */
/* Modbus write commands (mb_cmd.h)                                           */
/* -------------------------------------------------------------------------- */

/** @brief mbc_ops_t::lock. @param ctx Unused. */
static void mbc_lock(void *ctx) { (void)ctx; data_bus_lock(); }

/** @brief mbc_ops_t::unlock. @param ctx Unused. */
static void mbc_unlock(void *ctx) { (void)ctx; data_bus_unlock(); }

/**
 * @brief mbc_ops_t::points: the polling thread's register list.
 * @param ctx   Unused.
 * @param count Output: number of registers.
 * @return The registers.
 */
static const ModbusPoint *mbc_points(void *ctx, int *count)
{
    (void)ctx;
    *count = data_get_count();
    return data_get_points();
}

/**
 * @brief mbc_ops_t::write: FC05/FC06 for one value, FC15/FC16 otherwise.
 * @param ctx    Unused.
 * @param slave  Slave address.
 * @param table  REG_COIL or REG_HOLDING.
 * @param addr   First address.
 * @param count  Number of values.
 * @param values The values.
 * @return 1 if the slave confirmed the write.
 */
static int mbc_write(void *ctx, int slave, RegType table, uint16_t addr,
                     int count, const uint16_t *values)
{
    (void)ctx;
    if (count == 1 && (table == REG_COIL || table == REG_HOLDING))
        return data_write_register(slave, table, addr, values[0]);
    return data_write_block(slave, table, addr, count, values);
}

/** @brief Bus access for mb_cmd_handle(): the polling thread's driver and register list. */
static const mbc_ops_t mbc_ops = { NULL, mbc_lock, mbc_unlock, mbc_points, mbc_write };

/**
 * @brief Executes a Modbus write command and publishes the response.
 *
 * Runs in the mosquitto network thread; waits for at most one register read
 * of the polling thread (data_bus_lock()).
 *
 * @param m   Mosquitto instance.
 * @param msg The message.
 */
static void handle_mb_command(struct mosquitto *m, const struct mosquitto_message *msg)
{
    char resp[MB_CMD_RESPONSE_MAX];
    char resp_topic[160];

    mbc_result_t r = mb_cmd_handle(&mbc_ops, (const char *)msg->payload,
                                   msg->payloadlen > 0 ? (size_t)msg->payloadlen : 0,
                                   msg->retain, resp, sizeof(resp));

    printf("[MB_CMD] %s: %s\n", r == MBC_OK ? "written" : "not written", resp);

    snprintf(resp_topic, sizeof(resp_topic), MB_CMD_RESPONSE_TOPIC_FMT, cfg.device_id);
    mosquitto_publish(m, NULL, resp_topic, (int)strlen(resp), resp, 1, false);
}

/**
 * @brief Message callback: configuration pushes, Modbus write commands and (when enabled) OTA commands.
 *
 * @param m   Mosquitto instance.
 * @param ud  User data.
 * @param msg The message.
 */
static void on_message(struct mosquitto *m, void *ud, const struct mosquitto_message *msg)
{
    char config_topic[160];
    char cmd_topic[160];

    if (!msg || !msg->topic)
        return;

    snprintf(config_topic, sizeof(config_topic), CONFIG_SET_TOPIC_FMT, cfg.device_id);
    snprintf(cmd_topic, sizeof(cmd_topic), MB_CMD_TOPIC_FMT, cfg.device_id);

    if (strcmp(msg->topic, config_topic) == 0)
        handle_config_push(m, msg);
    else if (strcmp(msg->topic, cmd_topic) == 0)
        handle_mb_command(m, msg);
    else if (cfg.ota_enable)
        ota_on_message(m, ud, msg);
}

/**
 * @brief MQTT connection callback.
 *
 * Invoked when the broker accepts or rejects a connection request.
 * Updates the connection status and starts offline data replay when
 * appropriate.
 *
 * @param m Pointer to the Mosquitto client instance.
 * @param ud User-defined data pointer.
 * @param rc MQTT connection result code (0 indicates success).
 */
static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)m;
    (void)ud;

    atomic_store(&mqtt_connected, (rc == 0) ? 1 : 0);

    if (rc == 0) {
        printf("[MQTT] Connected to AWS IoT Core\n");

        /* Configuration pushed by the Smart RTU tool (retained: delivered on every connect). */
        char config_topic[160];
        snprintf(config_topic, sizeof(config_topic), CONFIG_SET_TOPIC_FMT, cfg.device_id);
        mosquitto_subscribe(m, NULL, config_topic, 1);
        printf("[MQTT] Subscribed to %s\n", config_topic);

        /* Modbus write commands (not retained: a retained one is answered, never executed). */
        char cmd_topic[160];
        snprintf(cmd_topic, sizeof(cmd_topic), MB_CMD_TOPIC_FMT, cfg.device_id);
        mosquitto_subscribe(m, NULL, cmd_topic, 1);
        printf("[MQTT] Subscribed to %s\n", cmd_topic);

        /* Subscribe to OTA command topics if enabled. */
        if (cfg.ota_enable)
        {
            mosquitto_subscribe(m, NULL, cfg.ota_app_topic, 1);
            mosquitto_subscribe(m, NULL, cfg.ota_system_topic, 1);

            printf("[MQTT] Subscribed to OTA topics\n");
        }

        /* Start offline replay now that we are connected. */
        offline_replay_start();

    } else {
        fprintf(stderr, "[MQTT] Connection failed (code %d)\n", rc);
    }
}

/**
 * @brief MQTT disconnect callback.
 *
 * Updates the connection status when the broker disconnects.
 *
 * @param m Pointer to the Mosquitto client instance.
 * @param ud User-defined data pointer.
 * @param rc Disconnect reason code.
 */
static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m;
    (void)ud;

    atomic_store(&mqtt_connected, 0);

    if (rc == 0)
        printf("[MQTT] Disconnected (clean)\n");
    else
        fprintf(stderr, "[MQTT] Disconnected unexpectedly (rc=%d)\n", rc);
}

/**
 * @brief Returns the Mosquitto client instance.
 *
 * @return Pointer to the active client, or NULL.
 */
struct mosquitto *mqtt_get_mosq(void)
{
    return mosq;
}

/*
 * @brief Publishes a payload to an arbitrary MQTT topic.
 *
 * @param topic   MQTT topic.
 * @param payload Null-terminated payload string.
 * @param qos     QoS level.
 *
 * @return 1 on success, 0 on failure.
 */
int mqtt_publish_to(const char *topic, const char *payload, int qos)
{
    if (!mosq || !atomic_load(&mqtt_connected) || !topic || !payload)
        return 0;

    int rc = mosquitto_publish(mosq,
                               NULL,
                               topic,
                               (int)strlen(payload),
                               payload,
                               qos,
                               false);

    return (rc == MOSQ_ERR_SUCCESS) ? 1 : 0;
}

/**
 * @brief Called by the connectivity monitor when the internet is up but MQTT is not connected.
 *
 * Re-registers the disconnect callback. Does nothing when mqtt_init() failed (no
 * broker configured, certificate error): there is no client then, and using the
 * NULL client crashed the application. The mosquitto network loop reconnects an
 * existing client by itself (mosquitto_reconnect_delay_set()).
 */
void reconnect_mqtt(void)
{
    if (!mosq)
        return;

    mosquitto_disconnect_callback_set(mosq, on_disconnect);
}

/**
 * @brief Initializes the MQTT client and connects to AWS IoT Core.
 *
 * Creates a Mosquitto client instance, configures X.509 mutual TLS
 * authentication, registers callbacks, connects to the AWS IoT Core
 * endpoint, and starts the network loop.
 *
 * @return 1 on success, 0 on failure.
 */
int mqtt_init(void)
{
    mosquitto_lib_init();

    /* Use configured client_id, or generate one if empty */

    const char *cid = cfg.mqtt_client_id;
    char cid_buf[128];

    if (cid[0] == '\0')
    {
        snprintf(cid_buf, sizeof(cid_buf),
                 "modbus_%s_%ld",
                 cfg.device_id,
                 (long)time(NULL));
        cid = cid_buf;
    }

    mosq = mosquitto_new(cid, true, NULL);

    if (!mosq)
        return 0;

    /* ------------------------------------------------------------------ */
    /* AWS IoT Core mutual TLS authentication                             */
    /* No username/password — authentication via X.509 certificates.      */
    /* ------------------------------------------------------------------ */

    if (mosquitto_tls_set(mosq,
                          cfg.mqtt_ca_cert,       /* CA certificate       */
                          NULL,                    /* CA path (unused)     */
                          cfg.mqtt_device_cert,    /* client certificate   */
                          cfg.mqtt_private_key,    /* private key          */
                          NULL                     /* password callback    */
                          ) != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "[MQTT] TLS certificate configuration failed\n");
        fprintf(stderr, "[MQTT]   CA:   %s\n", cfg.mqtt_ca_cert);
        fprintf(stderr, "[MQTT]   Cert: %s\n", cfg.mqtt_device_cert);
        fprintf(stderr, "[MQTT]   Key:  %s\n", cfg.mqtt_private_key);

        mosquitto_destroy(mosq);
        mosq = NULL;

        return 0;
    }

    /* AWS IoT Core requires TLS 1.2 minimum */

    mosquitto_tls_opts_set(mosq, 1, "tlsv1.2", NULL);

    /* Exponential backoff: 5s initial, max 60s, no jitter. */
    mosquitto_reconnect_delay_set(mosq, 5, 60, false);

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    mosquitto_publish_callback_set(mosq, on_publish);
    mosquitto_message_callback_set(mosq, on_message);

    printf("[MQTT] Connecting to %s:%d (client: %s)\n",
           cfg.mqtt_broker,
           cfg.mqtt_port,
           cid);

    if (mosquitto_connect(mosq,
                          cfg.mqtt_broker,
                          cfg.mqtt_port,
                          60) != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr, "[MQTT] Connect to %s:%d failed\n",
                cfg.mqtt_broker,
                cfg.mqtt_port);

        mosquitto_destroy(mosq);
        mosq = NULL;

        return 0;
    }

    mosquitto_loop_start(mosq);

    /* Allow time for the TLS handshake. */
    sleep(2);

    return 1;
}

/*
 * @brief Builds a JSON payload containing Modbus point values.
 *
 * Creates a JSON message consisting of the current timestamp and all
 * valid Modbus data points.
 *
 * @param buf Buffer to store the generated JSON payload.
 * @param buflen Size of the destination buffer in bytes.
 */
void build_payload(char *buf, size_t buflen)
{
    payload_build(buf, buflen, (long long)time(NULL) * 1000,
                  data_get_points(), data_get_count());
}

/*
 * @brief Publishes a payload to the configured MQTT topic.
 *
 * If the MQTT client is connected and internet connectivity is
 * available, the payload is published immediately. Otherwise, it is
 * stored for later transmission.
 *
 * @param payload Null-terminated JSON payload to publish.
 */
void mqtt_publish(const char *payload)
{
    if (atomic_load(&mqtt_connected) && atomic_load(&internet_up)) {

        if (mosquitto_publish(mosq,
                              NULL,
                              cfg.mqtt_topic,
                              (int)strlen(payload),
                              payload,
                              1,
                              false) == MOSQ_ERR_SUCCESS) {

            printf("[MQTT] Published %zu bytes to %s\n",
                   strlen(payload),
                   cfg.mqtt_topic);
            return;
        }
    }

    /* Store data for later transmission if publish fails. */
    offline_store(payload);
}

/**
 * @brief Releases all MQTT resources.
 *
 * Stops the Mosquitto network loop, destroys the client instance,
 * and cleans up the Mosquitto library.
 */
void mqtt_cleanup(void)
{
    if (mosq) {
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        mosq = NULL;
    }
}
