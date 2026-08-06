/**
 * @file ota.h
 * @brief Over-The-Air (OTA) Update Service
 *
 * OTA runs as an independent background daemon (ota_service),
 * separate from the main gateway application. It keeps its own
 * MQTT session to ThingsBoard and reacts to RPC commands.
 *
 * OTA workflow:
 *
 * ThingsBoard RPC ("fw_update")
 *        |
 *        v
 * ACK request immediately (RPC has a short timeout)
 *        |
 *        v
 * Spawn update worker thread
 *        |
 *        v
 * Download latest.json -> Compare Version
 *        |
 *        v
 * Download Firmware + SHA256 -> Verify
 *        |
 *        v
 * Backup Current Application
 *        |
 *        v
 * Install New Version
 *        |
 *        v
 * Restart gateway.service (or reboot board)
 *        |
 *        +-- Health check OK   -> SUCCESS
 *        +-- Health check FAIL -> Rollback -> FAILED
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
#define OTA_REQID_LEN        64

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
char mqtt_username[64];
char mqtt_password[64];
char mqtt_client_id[64];
char mqtt_topic[128];

    /* If true, reboot the whole STM32MP1 board after a successful
     * install. If false (default) only "gateway.service" is
     * restarted via systemd, which is faster and less disruptive. */
    bool reboot_after_update;

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

    pthread_t thread;          /* MQTT / RPC listener thread   */

    pthread_t worker_thread;   /* update pipeline worker thread */

    pthread_mutex_t lock;      /* guards update_in_progress/mosq */

    bool running;

    bool mqtt_connected;

    bool update_requested;

    bool update_in_progress;

    char pending_req_id[OTA_REQID_LEN];

    struct mosquitto *mosq;

} ota_context_t;

/*=============================================================
 * Public API
 *============================================================*/

int ota_init(void);

int ota_start(void);

void ota_stop(void);

ota_state_t ota_get_state(void);

const char *ota_state_string(ota_state_t state);

ota_context_t *ota_get_context(void);

int ota_read_current_version(void);

/**
 * Compare two "major.minor.patch" version strings.
 * @retval  1  latest > current (update available)
 * @retval  0  latest == current
 * @retval -1  latest < current, or a parse error occurred
 */
int ota_compare_version(const char *current, const char *latest);

/**
 * Thread-safe publish helper used by every OTA module.
 * Publishes `payload` on `topic` using the OTA service's own
 * mosquitto client (g_ota.mosq). This is intentionally distinct
 * from the main gateway's mqtt_publish() in src/mqtt.c, because
 * the OTA service is a separate process with its own MQTT session.
 *
 * @retval 0  Success
 * @retval -1 Not connected / publish failed
 */
int ota_mqtt_publish(const char *topic, const char *payload);

/*=============================================================
 * MQTT Status Publishing (ThingsBoard telemetry topics)
 *============================================================*/

int ota_publish_status(const char *status);

int ota_publish_progress(int percent);

int ota_publish_result(bool success);

/**
 * Run the full download -> verify -> install -> restart pipeline.
 * Safe to call from the worker thread only.
 *
 * @retval 0  Success (or already up to date)
 * @retval -1 Failure (state left as OTA_STATE_FAILED)
 */
int ota_execute_update(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_H */
