#ifndef MQTT_H
#define MQTT_H

/* ============================================================================
 * MQTT MODULE
 *
 * HiveMQ Cloud TLS connection via libmosquitto.
 * Offline payloads are spooled to SQLite so nothing is lost when the
 * broker is unreachable.
 * ========================================================================== */

/* Initialise SQLite offline store; returns 1 on success */
int  db_init(void);

/* Store payload in offline DB (called automatically by mqtt_publish) */
void db_store(const char *payload);

/* Initialise mosquitto, set TLS, connect, start loop; returns 1 on success */
int  mqtt_init(void);

/* Publish payload to MQTT_TOPIC; falls back to db_store on failure */
void mqtt_publish(const char *payload);

/* Graceful shutdown */
void mqtt_cleanup(void);

#endif /* MQTT_H */