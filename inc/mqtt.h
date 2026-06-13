#ifndef MQTT_H
#define MQTT_H

#include <stddef.h>   /* size_t */

#define MQTT_TOPIC      "modbus/data"
#define PAYLOAD_MAX     131072

extern volatile int mqtt_connected;


int mqtt_init(void);

void build_payload(char *buf, size_t buflen);

void mqtt_publish(const char *payload);

void mqtt_cleanup(void);

void reconnect_mqtt(void);

#endif /* MQTT_H */
