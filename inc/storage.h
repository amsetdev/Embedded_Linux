/**
 * @file storage.h
 * @brief Offline MQTT message storage interface.
 *
 * This module provides functions for storing MQTT payloads locally
 * when the network or MQTT broker is unavailable. Stored payloads
 * are replayed automatically when connectivity is restored.
 */

#ifndef STORAGE_H
#define STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief SQLite database used for offline MQTT storage.
 */
#define MQTT_STORAGE_DB "mqtt_storage.db"

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the offline storage subsystem.
 *
 * Creates the storage directory and prepares the offline
 * message storage mechanism.
 *
 * @return
 * - 1 on success.
 * - 0 on failure.
 */
int offline_init(void);

/**
 * @brief Stores an MQTT payload for later transmission.
 *
 * Called when the MQTT broker is unavailable or Internet
 * connectivity is lost.
 *
 * @param payload JSON payload to store.
 */
void offline_store(const char *payload);

/**
 * @brief Starts the offline replay thread.
 *
 * The replay thread periodically checks for stored payloads
 * and publishes them once MQTT connectivity is restored.
 */
void offline_replay_start(void);

/**
 * @brief Stops the offline replay thread and releases resources.
 */
void offline_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */