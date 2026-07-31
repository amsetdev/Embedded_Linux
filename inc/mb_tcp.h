/**
 * @file mb_tcp.h
 * @brief Modbus TCP SLAVE (server) interface.
 *
 * This module implements a Modbus TCP slave/server using libmodbus.
 * It listens for incoming master connections and serves the shared
 * register map (mb_regmap.h) — the same data model the RTU slave
 * exposes over RS485.
 */

#ifndef MODBUS_SLAVE_TCP_H
#define MODBUS_SLAVE_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Modbus TCP Slave Configuration                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief TCP port this slave listens on.
 */
#define MODBUS_DEFAULT_PORT         502

/**
 * @brief This device's Modbus slave ID (Unit Identifier).
 *        A master must address requests to this ID (or 0xFF/broadcast,
 *        which libmodbus accepts for TCP).
 */
#define MODBUS_DEFAULT_SLAVE_ID     1

/**
 * @brief Maximum number of simultaneously connected masters.
 */
#define MODBUS_TCP_MAX_CLIENTS      5

/**
 * @brief SQLite database file path used to log write requests
 *        received from external masters.
 */
#define DB_PATH                     "/tmp/modbus_data.db"

/* -------------------------------------------------------------------------- */
/* MQTT Configuration (unchanged — still used for status publishing)         */
/* -------------------------------------------------------------------------- */

#define MQTT_BROKER                  "25d1470809e1409796c6dd8bd937c33c.s1.eu.hivemq.cloud"
#define MQTT_PORT                   8883
#define MQTT_TOPIC                  "modbus/data"
#define MQTT_USERNAME               "Aishwarya"
#define MQTT_PASSWORD               "password"
#define MQTT_CLIENT_ID              "stm32mp157_modbus_slave"
#define MQTT_KEEPALIVE              60

/* -------------------------------------------------------------------------- */
/* Data Types                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus TCP slave thread configuration.
 *
 * Kept as an argument struct (same shape as the former master
 * config) so main.c's thread-creation code barely changes.
 */
typedef struct
{
    /** @brief Local TCP bind address ("0.0.0.0" for all interfaces). */
    char bind_ip[64];

    /** @brief TCP port to listen on (0 = use MODBUS_DEFAULT_PORT). */
    int listen_port;

    /** @brief This device's Modbus slave ID (0 = use MODBUS_DEFAULT_SLAVE_ID). */
    int slave_id;

} mb_thread_arg_t;

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus TCP slave thread entry function.
 *
 * Initializes the SQLite write-log database, opens a listening TCP
 * socket, and serves incoming master connections from the shared
 * register map until the application's running flag is cleared.
 *
 * @param arg Pointer to an ::mb_thread_arg_t structure.
 *
 * @return Always returns NULL when the thread exits.
 */
void *mb_thread_func1(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_SLAVE_TCP_H */
