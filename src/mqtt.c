#include "mqtt.h"
#include "settings.h"
#include "data.h"
#include "storage.h"
#include "connection.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mosquitto.h>

/** @brief Indicates whether the MQTT client is currently connected. */
volatile int mqtt_connected = 0;

/** @brief Mosquitto client instance. */
static struct mosquitto *mosq = NULL;

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

    mqtt_connected = (rc == 0);

    if (rc == 0) {
        printf("[ MQTT ] Connected\n");

        /* Start upload thread if required. */

    } else if (!mqtt_connected) {

        /* Replay offline data when connection becomes available. */
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
    (void)rc;

    mqtt_connected = 0;
    printf("[MQTT] Disconnected\n");
}

/**
 * @brief Registers the MQTT disconnect callback.
 */
void reconnect_mqtt(void)
{
    mosquitto_disconnect_callback_set(mosq, on_disconnect);
}

/**
 * @brief Initializes the MQTT client and connects to the broker.
 *
 * Creates a Mosquitto client instance, configures authentication and
 * TLS settings, registers callbacks, connects to the MQTT broker,
 * and starts the network loop.
 *
 * @return 1 on success, 0 on failure.
 */
int mqtt_init(void)
{
    mosquitto_lib_init();

    char cid[64];
    snprintf(cid, sizeof(cid), "modbus_%ld", (long)time(NULL));

    mosq = mosquitto_new(cid, true, NULL);
    if (!mosq)
        return 0;

    mosquitto_username_pw_set(mosq, cfg.mqtt_user, cfg.mqtt_pass);

    mosquitto_tls_set(mosq,
                      "/etc/ssl/certs/ca-certificates.crt",
                      NULL,
                      NULL,
                      NULL,
                      NULL);

    mosquitto_tls_opts_set(mosq, 1, NULL, NULL);

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    if (mosquitto_connect(mosq,
                          cfg.mqtt_broker,
                          cfg.mqtt_port,
                          60) != MOSQ_ERR_SUCCESS)
        return 0;

    mosquitto_loop_start(mosq);

    /* Allow time for the connection handshake. */
    sleep(2);

    return 1;
}

/**
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
    long long ts = (long long)time(NULL) * 1000;

    int pos = snprintf(buf, buflen, "{\"ts\":%lld,\"values\":{", ts);
    int first = 1;

    ModbusPoint *points = data_get_points();
    int count = data_get_count();

    for (int i = 0; i < count && pos < (int)buflen - 128; i++) {

        if (!points[i].valid)
            continue;

        if (!first)
            buf[pos++] = ',';

        pos += snprintf(buf + pos,
                        buflen - pos,
                        "\"%s\":%d",
                        points[i].label,
                        points[i].value);

        first = 0;
    }

    snprintf(buf + pos, buflen - pos, "}}");
}

/**
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
    if (mqtt_connected && internet_up) {

        if (mosquitto_publish(mosq,
                              NULL,
                              MQTT_TOPIC,
                              (int)strlen(payload),
                              payload,
                              1,
                              false) == MOSQ_ERR_SUCCESS) {

            printf("[MQTT] Published %zu bytes\n", strlen(payload));
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