/**
 * @file mqtt.h
 * @brief MQTT client interface.
 *
 * This module provides functions for initializing an MQTT client,
 * building JSON payloads from Modbus data, publishing messages to
 * an MQTT broker, and cleaning up resources.
 */

#ifndef MQTT_H
#define MQTT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief MQTT topic used for publishing Modbus data.
 */
#define MQTT_TOPIC      "modbus/data"

/**
 * @brief Maximum size of the generated JSON payload.
 */
#define PAYLOAD_MAX     131072

/* -------------------------------------------------------------------------- */
/* Global Variables                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief MQTT connection status.
 *
 * - 1 = Connected to the MQTT broker.
 * - 0 = Disconnected.
 */
extern volatile int mqtt_connected;

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the MQTT client.
 *
 * Creates the MQTT client, configures authentication and TLS settings,
 * connects to the configured broker, and starts the MQTT network loop.
 *
 * @return
 * - 1 if initialization succeeds.
 * - 0 if initialization or connection fails.
 */
int mqtt_init(void);

/**
 * @brief Builds a JSON payload from the current Modbus data.
 *
 * Generates a JSON object containing the current timestamp and all
 * valid Modbus point values.
 *
 * @param buf Output buffer for the JSON payload.
 * @param buflen Size of the output buffer in bytes.
 */
void build_payload(char *buf, size_t buflen);

/**
 * @brief Publishes a JSON payload to the MQTT broker.
 *
 * If the MQTT client is connected and Internet connectivity is
 * available, the payload is published immediately. Otherwise, it
 * is stored for offline replay.
 *
 * @param payload JSON payload to publish.
 */
void mqtt_publish(const char *payload);

/**
 * @brief Cleans up MQTT resources.
 *
 * Stops the MQTT network loop, disconnects from the broker,
 * destroys the client instance, and releases library resources.
 */
void mqtt_cleanup(void);

/**
 * @brief Reconfigures MQTT disconnect handling.
 *
 * Updates the MQTT disconnect callback so the application can
 * respond appropriately when the broker connection is lost.
 */
void reconnect_mqtt(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_H */