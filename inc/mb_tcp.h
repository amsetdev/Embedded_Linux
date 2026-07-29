/**
 * @file mb_tcp.h
 * @brief Modbus TCP master interface.
 *
 * This module implements a Modbus TCP master that periodically reads
 * holding registers from a Modbus TCP slave, stores the acquired data
 * in an SQLite database, and can be integrated with MQTT for cloud
 * publishing.
 */

#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Modbus Configuration                                                       */
/* -------------------------------------------------------------------------- */

/**
 * @brief Default Modbus TCP port.
 */
#define MODBUS_DEFAULT_PORT         502

/**
 * @brief Default Modbus slave ID.
 */
#define MODBUS_DEFAULT_SLAVE_ID     1

/**
 * @brief Starting holding register address.
 */
#define MODBUS_START_ADDR           0

/**
 * @brief Number of holding registers to read.
 */
#define MODBUS_NUM_REGS             100

/**
 * @brief Response timeout in seconds.
 */
#define MODBUS_RESPONSE_TIMEOUT     2

/**
 * @brief Polling interval in seconds.
 */
#define POLL_INTERVAL_SEC           120

/**
 * @brief SQLite database file path.
 */
#define DB_PATH                     "/tmp/modbus_data.db"

/* -------------------------------------------------------------------------- */
/* MQTT Configuration                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief MQTT broker hostname.
 */
#define MQTT_BROKER                  "25d1470809e1409796c6dd8bd937c33c.s1.eu.hivemq.cloud"

/**
 * @brief MQTT broker port.
 */
#define MQTT_PORT                   8883

/**
 * @brief MQTT publish topic.
 */
#define MQTT_TOPIC                  "modbus/data"

/**
 * @brief MQTT username.
 */
#define MQTT_USERNAME               "Aishwarya"

/**
 * @brief MQTT password.
 */
#define MQTT_PASSWORD               "password"

/**
 * @brief MQTT client identifier.
 */
#define MQTT_CLIENT_ID              "stm32mp157_modbus_master"

/**
 * @brief MQTT keep-alive interval in seconds.
 */
#define MQTT_KEEPALIVE              60

/* -------------------------------------------------------------------------- */
/* Data Types                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus TCP thread configuration.
 *
 * This structure contains the connection parameters required by the
 * Modbus TCP polling thread.
 */
typedef struct
{
    /**
     * @brief Modbus slave IPv4 address.
     */
    char slave_ip[64];

    /**
     * @brief Modbus TCP port number.
     */
    int slave_port;

    /**
     * @brief Modbus slave ID (Unit Identifier).
     */
    int slave_id;

} mb_thread_arg_t;

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus TCP polling thread entry function.
 *
 * Initializes the SQLite database, connects to the configured Modbus
 * TCP slave, periodically reads holding registers, stores the values
 * in the database, and automatically reconnects if communication is
 * lost.
 *
 * @param arg Pointer to an ::mb_thread_arg_t structure.
 *
 * @return Always returns NULL when the thread exits.
 */
void *mb_thread_func1(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* MODBUS_MASTER_H */