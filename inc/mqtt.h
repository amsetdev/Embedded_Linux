#ifndef MQTT_H
#define MQTT_H

/**
 * mqtt.h — MQTT client (mosquitto) with TLS, connect/disconnect callbacks,
 *           JSON payload builder, and offline publish fallback.
 */

#include <stddef.h>   /* size_t */

/* ---- Compile-time MQTT defaults --------------------------------------- */
#define MQTT_TOPIC      "modbus/data"
#define PAYLOAD_MAX     131072

/* ---- State ------------------------------------------------------------- */
extern volatile int mqtt_connected;

/* ---- API --------------------------------------------------------------- */

/** Initialise mosquitto library and attempt broker connection.
 *  Uses cfg (AppSettings) for broker / port / credentials.
 *  Returns 1 on success, 0 on failure. */
int mqtt_init(void);

/** Build a JSON payload from the current points array.
 *  Writes into buf (size buflen).  Format:
 *    {"ts":<unix_ms>,"values":{"Label":value,...}} */
void build_payload(char *buf, size_t buflen);

/** Publish payload to MQTT_TOPIC.
 *  Falls back to offline_store() if not connected or publish fails. */
void mqtt_publish(const char *payload);

/** Disconnect and free mosquitto resources. */
void mqtt_cleanup(void);

#endif /* MQTT_H */
