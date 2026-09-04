#include "mqtt.h"
#include "settings.h"
#include "data.h"
#include "storage.h"
#include "connection.h"
#include "ota.h"

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
        printf("[MQTT] Connected to AWS IoT Core\n");

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
    (void)rc;

    mqtt_connected = 0;
    printf("[MQTT] Disconnected\n");
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

/**
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
    if (!mosq || !mqtt_connected || !topic || !payload)
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
 * @brief Registers the MQTT disconnect callback.
 */
void reconnect_mqtt(void)
{
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

    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    if (cfg.ota_enable)
    {
        mosquitto_message_callback_set(mosq, ota_on_message);
    }

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

        switch (points[i].data_type)
        {
        case 'b':
            pos += snprintf(buf + pos,
                            buflen - pos,
                            "\"%s\":%s",
                            points[i].label,
                            points[i].value ? "true" : "false");
            break;

        case 'f':
            pos += snprintf(buf + pos,
                            buflen - pos,
                            "\"%s\":%.2f",
                            points[i].label,
                            points[i].float_value);
            break;

        default:
            pos += snprintf(buf + pos,
                            buflen - pos,
                            "\"%s\":%d",
                            points[i].label,
                            points[i].value);
            break;
        }

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
