/**
 * @file mb_cmd.h
 * @brief Modbus write commands via MQTT.
 *
 * Handles incoming MQTT messages that request Modbus register
 * or coil writes. Supports single writes (FC05/FC06) and
 * block writes (FC15/FC16).
 */

#ifndef MB_CMD_H
#define MB_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mosquitto.h>

/**
 * @brief MQTT message callback for Modbus write commands.
 *
 * Checks whether the incoming message matches the configured
 * command topic. If so, parses the JSON payload and dispatches
 * the write to the fieldbus driver.
 *
 * @param m   Mosquitto client instance.
 * @param ud  User data (unused).
 * @param msg Received MQTT message.
 */
void mb_cmd_on_message(struct mosquitto *m,
                       void *ud,
                       const struct mosquitto_message *msg);

#ifdef __cplusplus
}
#endif

#endif /* MB_CMD_H */
