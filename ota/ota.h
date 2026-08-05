/**
 * @file ota.h
 * @brief Over-The-Air (OTA) Update Service
 *
 * OTA workflow:
 *
 * ThingsBoard RPC
 *        │
 *        ▼
 * MQTT Update Command
 *        │
 *        ▼
 * Download latest.json
 *        │
 *        ▼
 * Compare Version
 *        │
 *        ▼
 * Download Firmware
 *        │
 *        ▼
 * Verify SHA256
 *        │
 *        ▼
 * Backup Current Application
 *        │
 *        ▼
 * Install New Version
 *        │
 *        ▼
 * Restart gateway.service
 *        │
 *        ├── Success
 *        │
 *        └── Rollback (if health check fails)
 */

#ifndef OTA_H
#define OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <mosquitto.h>

/*=============================================================
 * Constants
 *============================================================*/

#define OTA_VERSION_LEN      32
#define OTA_URL_LEN          256
#define OTA_FILENAME_LEN     128
#define OTA_SHA256_LEN       65
#define OTA_PATH_LEN         256
#define OTA_TOPIC_LEN        128
#define OTA_STATUS_LEN       64

/*=============================================================
 * OTA States
 *============================================================*/

typedef enum
{
    OTA_STATE_IDLE = 0,
    OTA_STATE_CONNECTING,
    OTA_STATE_WAITING,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_BACKUP,
    OTA_STATE_INSTALLING,
    OTA_STATE_RESTARTING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
    OTA_STATE_ROLLBACK

} ota_state_t;

/*=============================================================
 * OTA Configuration
 *============================================================*/

typedef struct
{
    bool enabled;

    int check_interval;

    char latest_url[OTA_URL_LEN];

    char download_directory[OTA_PATH_LEN];

    char backup_directory[OTA_PATH_LEN];

    char mqtt_host[128];

    int mqtt_port;

    char mqtt_token[128];

} ota_config_t;

/*=============================================================
 * OTA Package Information
 *============================================================*/

typedef struct
{
    char current_version[OTA_VERSION_LEN];

    char latest_version[OTA_VERSION_LEN];

    char package_name[OTA_FILENAME_LEN];

    char package_url[OTA_URL_LEN];

    char sha256_url[OTA_URL_LEN];

    char sha256[OTA_SHA256_LEN];

    char package_path[OTA_PATH_LEN];

    char backup_path[OTA_PATH_LEN];

    bool update_available;

} ota_info_t;

/*=============================================================
 * OTA Runtime Context
 *============================================================*/

typedef struct
{
    ota_config_t config;

    ota_info_t info;

    ota_state_t state;

    pthread_t thread;

    bool running;

    bool mqtt_connected;

    bool update_requested;

    struct mosquitto *mosq;

} ota_context_t;

/*=============================================================
 * Public API
 *============================================================*/

/**
 * Initialize OTA service.
 */
int ota_init(void);

/**
 * Start OTA background thread.
 */
int ota_start(void);

/**
 * Stop OTA service.
 */
void ota_stop(void);

/**
 * Get current OTA state.
 */
ota_state_t ota_get_state(void);

/**
 * Convert state to string.
 */
const char *ota_state_string(ota_state_t state);

/**
 * Return OTA context.
 */
ota_context_t *ota_get_context(void);

/**
 * Read installed firmware version.
 */
int ota_read_version(char *version,
                     uint32_t size);

/*=============================================================
 * MQTT Status Publishing
 *============================================================*/

/**
 * Publish OTA status.
 */
int ota_publish_status(const char *status);

/**
 * Publish OTA download progress.
 */
int ota_publish_progress(int percent);

/**
 * Publish OTA result.
 */
int ota_publish_result(bool success);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */