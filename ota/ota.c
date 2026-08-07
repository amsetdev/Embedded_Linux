/**
 * @file ota.c
 * @brief OTA Service Manager
 *
 * Standalone daemon that:
 *  - Connects to HiveMQ Cloud with its own MQTT session
 *    (username/password + TLS, independent of the main gateway app)
 *  - Subscribes to an OTA command topic and waits for an "ota_update"
 *    trigger (plain string or JSON: {"command":"ota_update"} /
 *    {"method":"fw_update"})
 *  - Runs the full pipeline on a worker thread:
 *      download latest.json -> compare version -> download package
 *      + sha256 -> verify -> BACKUP current main -> INSTALL new
 *      package -> RESTART gateway.service (main) -> health check
 *      -> ROLLBACK to backup + restart if the health check fails
 *  - Publishes live status/progress/result on the OTA status topics
 *
 * ---------------------------------------------------------------
 * Connection state machine (fixes the CONNECTING-loop bug)
 * ---------------------------------------------------------------
 * mosquitto_connect() performs the TCP+TLS handshake and sends the
 * CONNECT packet, but it does NOT wait for the broker's CONNACK.
 * The connection is only really "up" once ota_on_connect() fires,
 * which happens *inside* mosquitto_loop(). If you call
 * mosquitto_connect() again before that CONNACK has arrived, you
 * tear down the socket mid-handshake and the CONNACK never comes -
 * producing an endless CONNECTING loop with no error ever printed.
 *
 * To avoid that, this file tracks three explicit states:
 *   OTA_LINK_DISCONNECTED - not connected, safe to call connect()
 *   OTA_LINK_CONNECTING   - connect() sent, waiting for CONNACK
 *   OTA_LINK_CONNECTED    - CONNACK received (ota_on_connect fired)
 * connect() is only ever called from OTA_LINK_DISCONNECTED, and a
 * bounded timeout (OTA_CONNACK_TIMEOUT_SEC) moves CONNECTING back
 * to DISCONNECTED if no CONNACK shows up in time.
 */

#include "ota.h"
#include "ota_paths.h"
#include "ota_network.h"
#include "ota_config.h"
#include "ota_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include <sys/stat.h>

#include <mosquitto.h>
#include <cjson/cJSON.h>

/*=============================================================
 * Private Variables
 *============================================================*/

static ota_context_t g_ota;

typedef enum
{
    OTA_LINK_DISCONNECTED = 0,
    OTA_LINK_CONNECTING,
    OTA_LINK_CONNECTED

} ota_link_state_t;

#define OTA_CONNACK_TIMEOUT_SEC   10
#define OTA_RECONNECT_DELAY_SEC   5

/*=============================================================
 * Private Function Prototypes
 *============================================================*/

static void *ota_thread(void *arg);
static void *ota_update_worker(void *arg);

static int  ota_mqtt_connect(void);
static void ota_mqtt_disconnect(void);
static int  ota_mqtt_subscribe(void);

static void ota_on_connect(struct mosquitto *mosq, void *userdata, int rc);
static void ota_on_disconnect(struct mosquitto *mosq, void *userdata, int rc);
static void ota_on_message(struct mosquitto *mosq, void *userdata,
                            const struct mosquitto_message *msg);

static void ota_set_state(ota_state_t state);
static void ota_reset_info(void);
static int  ota_create_client(void);

/*=============================================================
 * Public Helper Functions
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
 * Read Current Firmware Version
 *============================================================*/

int ota_read_current_version(void)
{
    FILE *fp;

    memset(g_ota.info.current_version, 0, sizeof(g_ota.info.current_version));

    printf("OTA: Reading version file: %s\n", OTA_VERSION_FILE);

    fp = fopen(OTA_VERSION_FILE, "r");

    if (fp == NULL)
    {
        printf("OTA: Unable to open %s\n", OTA_VERSION_FILE);
        return -1;
    }

    if (fgets(g_ota.info.current_version,
              sizeof(g_ota.info.current_version),
              fp) == NULL)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    g_ota.info.current_version[
        strcspn(g_ota.info.current_version, "\r\n")] = '\0';

    printf("OTA: Current Version : %s\n",
           g_ota.info.current_version);

    return 0;
}

/*=============================================================
 * OTA Initialization
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
 * Start / Stop OTA Service
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
 * State / Info helpers
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
 * MQTT client / connection management
 *============================================================*/

static int ota_create_client(void)
{
    const char *client_id = g_ota.config.mqtt_client_id[0] != '\0'
                                 ? g_ota.config.mqtt_client_id
                                 : "stm32mp1-ota-service";

    g_ota.mosq = mosquitto_new(client_id, true, &g_ota);

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

    /* HiveMQ Cloud requires username+password and a valid TLS
     * handshake on port 8883. */
    int rc = mosquitto_username_pw_set(g_ota.mosq,
                                        g_ota.config.mqtt_username,
                                        g_ota.config.mqtt_password);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: Username/password set failed (%d): %s\n", rc, mosquitto_strerror(rc));
        return -1;
    }

    rc = mosquitto_tls_set(g_ota.mosq, "/etc/ssl/certs/ca-certificates.crt",
                            NULL, NULL, NULL, NULL);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: TLS set failed (%d): %s\n", rc, mosquitto_strerror(rc));
        return -1;
    }

    mosquitto_tls_opts_set(g_ota.mosq, 1, NULL, NULL);

    /* Blocking TCP+TLS handshake + sends CONNECT. Does NOT wait for
     * CONNACK -- that is picked up later by mosquitto_loop() and
     * reported through ota_on_connect(). */
    rc = mosquitto_connect(g_ota.mosq, g_ota.config.mqtt_host, g_ota.config.mqtt_port, 60);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: Connect failed (%d): %s\n", rc, mosquitto_strerror(rc));

        if (rc == MOSQ_ERR_ERRNO)
        {
            printf("OTA: errno detail: %s\n", strerror(errno));
        }

        return -1;
    }

    printf("OTA: TCP/TLS handshake sent to %s:%d as %s, waiting for CONNACK...\n",
           g_ota.config.mqtt_host, g_ota.config.mqtt_port, g_ota.config.mqtt_username);

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
    const char *topic = g_ota.config.mqtt_topic[0] != '\0'
                             ? g_ota.config.mqtt_topic
                             : "gateway/ota/command";

    int rc = mosquitto_subscribe(g_ota.mosq, NULL, topic, 1);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: Subscribe failed (%d): %s\n", rc, mosquitto_strerror(rc));
        return -1;
    }

    ota_set_state(OTA_STATE_WAITING);

    printf("OTA: Subscribed to '%s', waiting for OTA command...\n", topic);

    return 0;
}

/*=============================================================
 * Thread-safe publish helper (used everywhere)
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
 * OTA Thread (MQTT connection state machine + RPC loop)
 *============================================================*/

static void *ota_thread(void *arg)
{
    (void)arg;

    ota_link_state_t link = OTA_LINK_DISCONNECTED;
    time_t connecting_since = 0;
    bool subscribed = false;

    printf("OTA: Thread started\n");

    if (ota_create_client() != 0)
    {
        printf("OTA: Failed to create MQTT client\n");
        return NULL;
    }

    while (g_ota.running)
    {
        switch (link)
        {
            case OTA_LINK_DISCONNECTED:

                subscribed = false;

                if (ota_mqtt_connect() != 0)
                {
                    sleep(OTA_RECONNECT_DELAY_SEC);
                    break; /* stay DISCONNECTED, try again next pass */
                }

                link = OTA_LINK_CONNECTING;
                connecting_since = time(NULL);
                break;

            case OTA_LINK_CONNECTING:

                /* Give the broker a chance to answer. Do NOT call
                 * mosquitto_connect() again while we're in this
                 * state -- that is exactly what caused the endless
                 * CONNECTING loop. */
                mosquitto_loop(g_ota.mosq, 1000, 1);

                if (g_ota.mqtt_connected)
                {
                    link = OTA_LINK_CONNECTED;
                    break;
                }

                if (time(NULL) - connecting_since > OTA_CONNACK_TIMEOUT_SEC)
                {
                    printf("OTA: No CONNACK within %d s, retrying connection\n",
                           OTA_CONNACK_TIMEOUT_SEC);

                    mosquitto_disconnect(g_ota.mosq);
                    link = OTA_LINK_DISCONNECTED;
                    sleep(OTA_RECONNECT_DELAY_SEC);
                }

                break;

            case OTA_LINK_CONNECTED:

                mosquitto_loop(g_ota.mosq, 1000, 1);

                if (!g_ota.mqtt_connected)
                {
                    /* ota_on_disconnect() already logged the reason. */
                    link = OTA_LINK_DISCONNECTED;
                    sleep(2);
                    break;
                }

                if (!subscribed)
                {
                    if (ota_mqtt_subscribe() == 0)
                    {
                        subscribed = true;
                    }
                }

                break;
        }

        usleep(100000);
    }

    mosquitto_disconnect(g_ota.mosq);
    mosquitto_destroy(g_ota.mosq);
    g_ota.mosq = NULL;

    return NULL;
}

/*=============================================================
 * MQTT Callbacks
 *============================================================*/

static void ota_on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;

    if (rc == 0)
    {
        g_ota.mqtt_connected = true;
        printf("OTA: MQTT Connected to HiveMQ (CONNACK received)\n");
    }
    else
    {
        g_ota.mqtt_connected = false;
        printf("OTA: MQTT Connection Rejected (%d): %s\n",
               rc, mosquitto_connack_string(rc));
    }
}

static void ota_on_disconnect(struct mosquitto *mosq, void *userdata, int rc)
{
    (void)mosq;
    (void)userdata;

    g_ota.mqtt_connected = false;
    printf("OTA: MQTT Disconnected (rc=%d): %s\n", rc, mosquitto_strerror(rc));
}

/**
 * @brief Receive OTA command over MQTT.
 *
 * Accepts either:
 *  - a plain string "ota_update" (as typed in HiveMQ's web client)
 *  - JSON: {"command":"ota_update"} or {"method":"fw_update"}
 *
 * Hands the actual pipeline off to a worker thread so this callback
 * (running inside mosquitto_loop()) returns immediately.
 */
static void ota_on_message(struct mosquitto *mosq, void *userdata,
                            const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)userdata;

    if (msg == NULL || msg->payload == NULL || msg->payloadlen <= 0)
    {
        return;
    }

    char payload[256] = {0};
    int len = msg->payloadlen < (int)sizeof(payload) - 1
                  ? msg->payloadlen
                  : (int)sizeof(payload) - 1;
    memcpy(payload, msg->payload, len);
    payload[len] = '\0';

    printf("\nOTA MQTT Message Received\nTopic   : %s\nPayload : %s\n",
           msg->topic, payload);

    bool is_update_command = false;

    /* Plain-text command, e.g. typed directly in HiveMQ's web client. */
    if (payload[0] != '{' && strstr(payload, "ota_update") != NULL)
    {
        is_update_command = true;
    }
    else
    {
        /* JSON command, e.g. {"command":"ota_update"} */
        cJSON *root = cJSON_Parse(payload);

        if (root != NULL)
        {
            cJSON *cmd = cJSON_GetObjectItem(root, "command");
            cJSON *method = cJSON_GetObjectItem(root, "method");

            if ((cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ota_update") == 0) ||
                (cJSON_IsString(method) && strcmp(method->valuestring, "fw_update") == 0))
            {
                is_update_command = true;
            }

            cJSON_Delete(root);
        }
    }

    if (!is_update_command)
    {
        return;
    }

    pthread_mutex_lock(&g_ota.lock);

    if (g_ota.update_in_progress)
    {
        pthread_mutex_unlock(&g_ota.lock);
        printf("OTA: Update already in progress, ignoring new request\n");
        ota_mqtt_publish("gateway/ota/status", "{\"state\":\"BUSY\"}");
        return;
    }

    g_ota.update_in_progress = true;

    pthread_mutex_unlock(&g_ota.lock);

    printf("OTA Update Requested via MQTT\n");

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
 * Version Comparison
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
 * Update Worker Thread - full OTA pipeline
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

/**
 * Full pipeline: check -> download -> verify -> BACKUP -> INSTALL
 * -> RESTART main (gateway.service) -> health check -> ROLLBACK on
 * failure.
 *
 * Backup/rollback: ota_backup_current() copies the currently running
 * "main" + version.txt into ./backup BEFORE anything is overwritten.
 * If install fails, or if main fails its post-restart health check,
 * ota_restore_backup() puts that exact backup back and restarts
 * main again -- so a bad update never leaves the board bricked.
 */
int ota_execute_update(void)
{
    ota_context_t *ctx = ota_get_context();

    /* Always read the current version from disk before checking for updates */
    if (ota_read_current_version() != 0)
    {
        printf("OTA: Failed to read current version\n");
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    /* Clear previous OTA metadata */
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

    /* ---- Backup current "main" before touching anything ---- */
    ota_set_state(OTA_STATE_BACKUP);

    if (ota_backup_current() != 0)
    {
        printf("OTA: Backup failed, aborting update\n");
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    /* ---- Install new package ---- */
    ota_set_state(OTA_STATE_INSTALLING);

    if (ota_install_package(ctx->info.package_path) != 0)
    {
        printf("OTA: Install failed, restoring backup\n");
        ota_restore_backup();
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    /* Save the new version */
    FILE *vfp = fopen(OTA_VERSION_FILE, "w");
    if (vfp != NULL)
    {
        fprintf(vfp, "%s\n", ctx->info.latest_version);
        fclose(vfp);

        /* Keep RAM in sync with version.txt */
        strncpy(ctx->info.current_version,
                ctx->info.latest_version,
                sizeof(ctx->info.current_version) - 1);

        ctx->info.current_version[
            sizeof(ctx->info.current_version) - 1] = '\0';
    }

    /* ---- Restart service ---- */
    ota_set_state(OTA_STATE_RESTARTING);

    if (ctx->config.reboot_after_update)
    {
        ota_publish_result(true);
        ota_reboot_board();
        return 0;
    }

    if (ota_restart_service() != 0)
    {
        printf("OTA: Failed to restart gateway.service\n");
        ota_restore_backup();
        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    if (!ota_health_check())
    {
        printf("OTA: New version failed health check, rolling back to backup\n");

        ota_set_state(OTA_STATE_ROLLBACK);

        ota_restore_backup();
        ota_restart_service();

        ota_set_state(OTA_STATE_FAILED);
        return -1;
    }

    ota_set_state(OTA_STATE_SUCCESS);

    printf("OTA: Update to version %s completed successfully, main restarted\n",
           ctx->info.latest_version);

    return 0;
}