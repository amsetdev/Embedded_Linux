/**
 * @file ota.c
 * @brief OTA Service Manager
 *
 * This module is the heart of the standalone OTA daemon
 * (ota_service), which runs independently from the main
 * gateway application. Responsibilities:
 *
 *  - OTA initialization
 *  - ThingsBoard MQTT session (own client, own credentials)
 *  - RPC command handling ("fw_update")
 *  - OTA state machine
 *  - Firmware version management
 *  - Update worker thread (download/verify/backup/install/restart)
 *
 * Network download operations live in ota_network.c.
 * Installation and rollback live in ota_install.c.
 */

#include "ota.h"
#include "ota_paths.h"
#include "ota_network.h"
#include "ota_config.h"
#include "ota_install.h"
#include "mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/stat.h>

#include <mosquitto.h>
#include <cjson/cJSON.h>

/*=============================================================
 *                 Private Variables
 *============================================================*/

static ota_context_t g_ota;

/*=============================================================
 *             Private Function Prototypes
 *============================================================*/

static void *ota_thread(void *arg);
static void *ota_update_worker(void *arg);

static int ota_mqtt_connect(void);
static void ota_mqtt_disconnect(void);
static int ota_mqtt_subscribe(void);

static void ota_on_connect(struct mosquitto *mosq, void *userdata, int rc);
static void ota_on_disconnect(struct mosquitto *mosq, void *userdata, int rc);
static void ota_on_message(struct mosquitto *mosq, void *userdata,
                            const struct mosquitto_message *msg);

static void ota_set_state(ota_state_t state);
static void ota_reset_info(void);
static int ota_create_client(void);

/*=============================================================
 *               Public Helper Functions
 *============================================================*/

ota_context_t *ota_get_context(void)
{
    return &g_ota;
}

const char *ota_state_string(ota_state_t state)
{
    switch (state)
    {
        case OTA_STATE_IDLE:        return "IDLE";
        case OTA_STATE_CONNECTING:  return "CONNECTING";
        case OTA_STATE_WAITING:     return "WAITING";
        case OTA_STATE_CHECKING:    return "CHECKING";
        case OTA_STATE_DOWNLOADING: return "DOWNLOADING";
        case OTA_STATE_VERIFYING:   return "VERIFYING";
        case OTA_STATE_BACKUP:      return "BACKUP";
        case OTA_STATE_INSTALLING:  return "INSTALLING";
        case OTA_STATE_RESTARTING:  return "RESTARTING";
        case OTA_STATE_SUCCESS:     return "SUCCESS";
        case OTA_STATE_FAILED:      return "FAILED";
        case OTA_STATE_ROLLBACK:    return "ROLLBACK";
        default:                    return "UNKNOWN";
    }
}

/*=============================================================
 *           Read Current Firmware Version
 *============================================================*/

int ota_read_current_version(void)
{
    FILE *fp = fopen(OTA_VERSION_FILE, "r");

    if (fp == NULL)
    {
        printf("OTA: Unable to open %s\n", OTA_VERSION_FILE);
        return -1;
    }

    if (fgets(g_ota.info.current_version, sizeof(g_ota.info.current_version), fp) == NULL)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    g_ota.info.current_version[strcspn(g_ota.info.current_version, "\r\n")] = '\0';

    printf("OTA: Current Version : %s\n", g_ota.info.current_version);

    return 0;
}

/*=============================================================
 *                 OTA Initialization
 *============================================================*/

int ota_init(void)
{
    memset(&g_ota, 0, sizeof(g_ota));

    g_ota.state = OTA_STATE_IDLE;

    pthread_mutex_init(&g_ota.lock, NULL);

    printf("\n========================================\n");
    printf("        OTA SERVICE INITIALIZATION\n");
    printf("========================================\n");

    if (ota_load_config() != 0)
    {
        printf("OTA: Failed to load configuration\n");
        return -1;
    }

    /* Ensure working directories exist (first boot / fresh image). */
    mkdir(OTA_UPDATE_DIRECTORY, 0755);
    mkdir(OTA_BACKUP_DIRECTORY, 0755);
    mkdir(OTA_TEMP_DIRECTORY, 0755);

    if (ota_read_current_version() != 0)
    {
        printf("OTA: Failed to read current version\n");
        return -1;
    }

    mosquitto_lib_init();

    printf("Mosquitto Library Initialized\n");
    printf("OTA Initialization Complete\n\n");

    return 0;
}

/*=============================================================
 *               Start / Stop OTA Service
 *============================================================*/

int ota_start(void)
{
    if (g_ota.running)
    {
        printf("OTA Service Already Running\n");
        return 0;
    }

    g_ota.running = true;

    if (pthread_create(&g_ota.thread, NULL, ota_thread, NULL) != 0)
    {
        printf("OTA: Thread Creation Failed\n");
        g_ota.running = false;
        return -1;
    }

    printf("OTA Thread Started\n");

    return 0;
}

void ota_stop(void)
{
    if (!g_ota.running)
    {
        return;
    }

    printf("Stopping OTA Service...\n");

    g_ota.running = false;

    pthread_join(g_ota.thread, NULL);

    pthread_mutex_lock(&g_ota.lock);
    bool update_running = g_ota.update_in_progress;
    pthread_mutex_unlock(&g_ota.lock);

    if (update_running)
    {
        /* Let an in-flight firmware update finish rather than
         * tearing down mid-install, which could brick the board. */
        printf("OTA: Waiting for in-progress update to finish...\n");
        pthread_join(g_ota.worker_thread, NULL);
    }

    ota_mqtt_disconnect();
    mosquitto_lib_cleanup();
    pthread_mutex_destroy(&g_ota.lock);

    printf("OTA Service Stopped\n");
}

ota_state_t ota_get_state(void)
{
    return g_ota.state;
}

/*=============================================================
 *            State / Info helpers
 *============================================================*/

static void ota_set_state(ota_state_t state)
{
    g_ota.state = state;

    printf("OTA State -> %s\n", ota_state_string(state));

    ota_publish_status(ota_state_string(state));
}

static void ota_reset_info(void)
{
    memset(&g_ota.info.latest_version, 0, sizeof(g_ota.info.latest_version));
    memset(&g_ota.info.package_name, 0, sizeof(g_ota.info.package_name));
    memset(&g_ota.info.package_url, 0, sizeof(g_ota.info.package_url));
    memset(&g_ota.info.sha256_url, 0, sizeof(g_ota.info.sha256_url));
    memset(&g_ota.info.sha256, 0, sizeof(g_ota.info.sha256));
    memset(&g_ota.info.package_path, 0, sizeof(g_ota.info.package_path));
    memset(&g_ota.info.backup_path, 0, sizeof(g_ota.info.backup_path));

    g_ota.info.update_available = false;
}

/*=============================================================
 *            MQTT client / connection management
 *============================================================*/

static int ota_create_client(void)
{
    g_ota.mosq = mosquitto_new(NULL, true, &g_ota);

    if (g_ota.mosq == NULL)
    {
        printf("OTA: mosquitto_new() failed\n");
        return -1;
    }

    /* The update worker thread publishes status/progress/result
     * concurrently with mosquitto_loop() running on ota_thread,
     * so the client must be thread-safe. */
    mosquitto_threaded_set(g_ota.mosq, true);

    mosquitto_connect_callback_set(g_ota.mosq, ota_on_connect);
    mosquitto_disconnect_callback_set(g_ota.mosq, ota_on_disconnect);
    mosquitto_message_callback_set(g_ota.mosq, ota_on_message);

    return 0;
}

static int ota_mqtt_connect(void)
{
    ota_set_state(OTA_STATE_CONNECTING);


  int  rc = mosquitto_username_pw_set(
        g_ota.mosq,
        g_ota.config.mqtt_username,
        g_ota.config.mqtt_password);

if (rc != MOSQ_ERR_SUCCESS)
{
    printf("OTA: Username/password set failed (%s)\n",
           mosquitto_strerror(rc));
    return -1;
}

    mosquitto_tls_opts_set(g_ota.mosq, 1, NULL, NULL);

    rc = mosquitto_connect(g_ota.mosq, g_ota.config.mqtt_host, g_ota.config.mqtt_port, 60);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: Connect failed (%d)\n", rc);
        return -1;
    }

    printf("OTA: Connected to broker\n");

    return 0;
}

static void ota_mqtt_disconnect(void)
{
    if (g_ota.mosq != NULL)
    {
        mosquitto_disconnect(g_ota.mosq);
    }

    g_ota.mqtt_connected = false;
}

static int ota_mqtt_subscribe(void)
{
    int rc = mosquitto_subscribe(
        g_ota.mosq,
        NULL,
        g_ota.config.mqtt_topic,
        1);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: Subscribe failed\n");
        return -1;
    }

    ota_set_state(OTA_STATE_WAITING);

    printf("OTA: Waiting for OTA command...\n");

    return 0;
}

/*=============================================================
 *          Thread-safe publish helper (used everywhere)
 *============================================================*/

int ota_mqtt_publish(const char *topic, const char *payload)
{
    int rc;

    pthread_mutex_lock(&g_ota.lock);

    if (g_ota.mosq == NULL || !g_ota.mqtt_connected)
    {
        pthread_mutex_unlock(&g_ota.lock);
        return -1;
    }

    rc = mosquitto_publish(g_ota.mosq, NULL, topic,
                            (int)strlen(payload), payload, 1, false);

    pthread_mutex_unlock(&g_ota.lock);

    return (rc == MOSQ_ERR_SUCCESS) ? 0 : -1;
}

int ota_publish_status(const char *status)
{
    char payload[128];

    snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", status);

    return ota_mqtt_publish("gateway/ota/status", payload);
}

int ota_publish_progress(int percent)
{
    char payload[64];

    snprintf(payload, sizeof(payload), "{\"progress\":%d}", percent);

    return ota_mqtt_publish("gateway/ota/progress", payload);
}

int ota_publish_result(bool success)
{
    char payload[128];

    snprintf(payload, sizeof(payload), "{\"result\":\"%s\"}",
             success ? "SUCCESS" : "FAILED");

    return ota_mqtt_publish("gateway/ota/result", payload);
}

/*=============================================================
 *                 OTA Worker Thread (MQTT / RPC)
 *============================================================*/

static void *ota_thread(void *arg)
{
    (void)arg;

    printf("OTA: Thread started\n");

    if (ota_create_client() != 0)
    {
        printf("OTA: Failed to create MQTT client\n");
        return NULL;
    }

    while (g_ota.running)
    {
        if (!g_ota.mqtt_connected)
        {
            if (ota_mqtt_connect() != 0)
            {
                sleep(5);
                continue;
            }

            ota_mqtt_subscribe();
        }

        /* Keeps the keepalive/PINGREQ alive and delivers incoming
         * RPC messages. The actual firmware update runs on a
         * separate worker thread so this loop is never blocked
         * by a multi-minute download. */
        mosquitto_loop(g_ota.mosq, 1000, 1);

        usleep(100000);
    }

    mosquitto_destroy(g_ota.mosq);
    g_ota.mosq = NULL;

    return NULL;
}

/*=============================================================
 *              MQTT Callbacks
 *============================================================*/

static void ota_on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;

    if (rc == 0)
    {
        g_ota.mqtt_connected = true;
        printf("OTA: MQTT Connected\n");
    }
    else
    {
        printf("OTA: MQTT Connection Failed (%d)\n", rc);
    }
}

static void ota_on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;
    (void)rc;

    g_ota.mqtt_connected = false;
    printf("OTA: MQTT Disconnected\n");
}

/**
 * @brief Receive OTA RPC command from ThingsBoard.
 *
 * Expected ThingsBoard two-way RPC payload:
 *   { "method": "fw_update", "params": {} }
 *
 * ThingsBoard RPC calls time out after a few seconds, so this
 * callback never runs the update itself. It:
 *   1. Parses/validates the request with cJSON.
 *   2. Immediately ACKs on the RPC response topic.
 *   3. Hands off to a worker thread that runs the full pipeline
 *      and reports progress/result on the gateway/ota status topics.
 */
static void ota_on_message(struct mosquitto *mosq, void *userdata,
                            const struct mosquitto_message *msg)
{
    (void)userdata;

    if (msg == NULL || msg->payload == NULL)
    {
        return;
    }

    printf("\nOTA RPC Received\nTopic   : %s\nPayload : %.*s\n",
           msg->topic, msg->payloadlen, (char *)msg->payload);

    const char *prefix = "v1/devices/me/rpc/request/";

    if (strncmp(msg->topic, prefix, strlen(prefix)) != 0)
    {
        return;
    }

    const char *req_id = msg->topic + strlen(prefix);

    cJSON *root = cJSON_ParseWithLength((char *)msg->payload, (size_t)msg->payloadlen);

    if (root == NULL)
    {
        printf("OTA: RPC payload is not valid JSON\n");
        return;
    }

    cJSON *method = cJSON_GetObjectItem(root, "method");

    if (!cJSON_IsString(method) || strcmp(method->valuestring, "fw_update") != 0)
    {
        cJSON_Delete(root);
        return; /* not an OTA command, ignore */
    }

    cJSON_Delete(root);

    char resp_topic[OTA_TOPIC_LEN];
    snprintf(resp_topic, sizeof(resp_topic), "v1/devices/me/rpc/response/%s", req_id);

    pthread_mutex_lock(&g_ota.lock);

    if (g_ota.update_in_progress)
    {
        pthread_mutex_unlock(&g_ota.lock);

        printf("OTA: Update already in progress, ignoring new request\n");

        const char *busy = "{\"status\":\"busy\"}";
        mosquitto_publish(mosq, NULL, resp_topic, (int)strlen(busy), busy, 1, false);
        return;
    }

    g_ota.update_in_progress = true;
    strncpy(g_ota.pending_req_id, req_id, sizeof(g_ota.pending_req_id) - 1);

    pthread_mutex_unlock(&g_ota.lock);

    printf("OTA Update Requested (reqId=%s)\n", req_id);

    /* Ack right away so the RPC call doesn't time out on the
     * ThingsBoard side; the real result follows asynchronously
     * on gateway/ota/result once the pipeline finishes. */
    const char *ack = "{\"status\":\"started\"}";
    mosquitto_publish(mosq, NULL, resp_topic, (int)strlen(ack), ack, 1, false);

    if (pthread_create(&g_ota.worker_thread, NULL, ota_update_worker, NULL) != 0)
    {
        printf("OTA: Failed to spawn update worker thread\n");

        pthread_mutex_lock(&g_ota.lock);
        g_ota.update_in_progress = false;
        pthread_mutex_unlock(&g_ota.lock);
    }
    else
    {
        pthread_detach(g_ota.worker_thread);
    }
}

/*=============================================================
 *                 Version Comparison
 *============================================================*/

int ota_compare_version(const char *current, const char *latest)
{
    int c_major, c_minor, c_patch;
    int l_major, l_minor, l_patch;

    if (sscanf(current, "%d.%d.%d", &c_major, &c_minor, &c_patch) != 3)
    {
        return -1;
    }

    if (sscanf(latest, "%d.%d.%d", &l_major, &l_minor, &l_patch) != 3)
    {
        return -1;
    }

    if (l_major != c_major) return (l_major > c_major) ? 1 : -1;
    if (l_minor != c_minor) return (l_minor > c_minor) ? 1 : -1;
    if (l_patch != c_patch) return (l_patch > c_patch) ? 1 : -1;

    return 0;
}

/*=============================================================
 *      Update Worker Thread - full OTA pipeline
 *============================================================*/

static void *ota_update_worker(void *arg)
{
    (void)arg;

    int result = ota_execute_update();

    ota_publish_result(result == 0);

    pthread_mutex_lock(&g_ota.lock);
    g_ota.update_in_progress = false;
    pthread_mutex_unlock(&g_ota.lock);

    return NULL;
}

int ota_execute_update(void)
{
    ota_context_t *ctx = ota_get_context();

    /* ota_reset_info() clears stale latest_version/urls/hash from any
     * previous cycle without touching current_version. */
    ota_reset_info();

    ota_set_state(OTA_STATE_CHECKING);

    if (ota_download_latest_json() != 0 || ota_parse_latest_json() != 0)
    {
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    if (!ctx->info.update_available)
    {
        printf("OTA: Already running latest version.\n");
        ota_set_state(OTA_STATE_IDLE);
        return 0;
    }

    /* ---- Download & verify ---- */
    ota_set_state(OTA_STATE_DOWNLOADING);

    if (ota_download_package() != 0 || ota_download_sha256() != 0)
    {
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    ota_set_state(OTA_STATE_VERIFYING);

    if (ota_verify_package() != 0)
    {
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    /* ---- Backup current install before touching anything ---- */
    ota_set_state(OTA_STATE_BACKUP);

    if (ota_backup_current() != 0)
    {
        printf("OTA: Backup failed, aborting update\n");
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    /* ---- Install ---- */
    ota_set_state(OTA_STATE_INSTALLING);

    if (ota_install_package() != 0)
    {
        printf("OTA: Install failed, restoring backup\n");
        ota_restore_backup();
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    /* Record the new version so the next boot's ota_read_current_version()
     * reports it correctly. */
    FILE *vfp = fopen(OTA_VERSION_FILE, "w");
    if (vfp != NULL)
    {
        fprintf(vfp, "%s\n", ctx->info.latest_version);
        fclose(vfp);
    }

    /* ---- Restart / reboot + health check ---- */
    ota_set_state(OTA_STATE_RESTARTING);

    if (ctx->config.reboot_after_update)
    {
        /* This does not return on success -- the board reboots.
         * Health-check/rollback in this mode has to happen on the
         * *next* boot (e.g. a watchdog in ota_main checking that
         * gateway.service came up, outside the scope of this
         * process's lifetime). */
        ota_publish_result(true);
        ota_reboot_board();
        return 0;
    }

    ota_restart_service();

    if (!ota_health_check())
    {
        printf("OTA: New version failed health check, rolling back\n");

        ota_set_state(OTA_STATE_ROLLBACK);

        ota_restore_backup();
        ota_restart_service();

        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    ota_set_state(OTA_STATE_SUCCESS);

    printf("OTA: Update to version %s completed successfully\n",
           ctx->info.latest_version);

    return 0;
}
