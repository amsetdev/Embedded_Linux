/**
 * @file ota.h
 * @brief Over-The-Air update interface.
 *
 * Provides dual OTA support:
 * - Application OTA: downloads a new binary, verifies SHA256, replaces in place.
 * - System OTA: downloads an .swu image, invokes swupdate CLI.
 *
 * OTA commands arrive via MQTT. Status is reported back on a dedicated topic.
 */

#ifndef OTA_H
#define OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <mosquitto.h>

/* Types (ota_type_t, ota_status_t, ota_request_t) and the host-testable
 * decisions live in ota_logic.h. */
#include "ota_logic.h"

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the OTA subsystem.
 *
 * Creates the download directory and initializes synchronization
 * primitives.
 *
 * @return 1 on success, 0 on failure.
 */
int ota_init(void);

/**
 * @brief OTA worker thread function.
 *
 * Waits on a condition variable for incoming OTA requests and
 * processes them sequentially.
 *
 * @param arg Unused.
 * @return Always NULL.
 */
void *ota_thread_func(void *arg);

/**
 * @brief MQTT message callback for OTA commands.
 *
 * Parses incoming MQTT messages on OTA topics, populates the
 * request structure, and signals the OTA thread.
 *
 * @param m   Mosquitto client instance.
 * @param ud  User data (unused).
 * @param msg Received MQTT message.
 */
void ota_on_message(struct mosquitto *m,
                    void *ud,
                    const struct mosquitto_message *msg);

/**
 * @brief Sets the watchdog heartbeat ID for the OTA thread.
 *
 * @param id  Watchdog ID from watchdog_register().
 */
void ota_set_wdg_id(int id);

/**
 * @brief Cleans up the OTA subsystem.
 *
 * Signals the OTA thread to exit and releases resources.
 */
void ota_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
