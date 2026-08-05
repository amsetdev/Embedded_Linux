/**
 * @file ota.c
 * @brief OTA Service Manager
 *
 * This module implements the OTA manager for the Smart Gateway.
 * It is responsible for:
 *
 *  - OTA initialization
 *  - ThingsBoard MQTT communication
 *  - OTA state machine
 *  - Firmware version management
 *  - OTA worker thread
 *  - Executing firmware updates
 *
 * Network download operations are implemented in
 * ota_network.c.
 *
 * Installation and rollback are implemented in
 * ota_install.c.
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

#include <sys/stat.h>

#include <mosquitto.h>

/*=============================================================
 *                 Private Variables
 *============================================================*/

/**
 * @brief Global OTA context.
 */
static ota_context_t g_ota;

/*=============================================================
 *             Private Function Prototypes
 *============================================================*/

static void *ota_thread(void *arg);

static int ota_execute_update(void);

static int ota_mqtt_connect(void);

static void ota_mqtt_disconnect(void);

static int ota_mqtt_subscribe(void);

static void ota_on_connect(struct mosquitto *mosq,
                           void *userdata,
                           int rc);

static void ota_on_disconnect(struct mosquitto *mosq,
                              void *userdata,
                              int rc);

static  void ota_on_message(struct mosquitto *mosq,
                           void *userdata,
                           const struct mosquitto_message *msg);

  int ota_read_current_version(void);

  int ota_compare_version(const char *current,
                               const char *latest);

/*=============================================================
 *               Public Helper Functions
 *============================================================*/

ota_context_t *ota_get_context(void)
{
    return &g_ota;
}

/*=============================================================
 *              OTA State String
 *============================================================*/

const char *ota_state_string(ota_state_t state)
{
    switch (state)
    {
        case OTA_STATE_IDLE:
            return "IDLE";

        case OTA_STATE_CONNECTING:
            return "CONNECTING";

        case OTA_STATE_WAITING:
            return "WAITING";

        case OTA_STATE_CHECKING:
            return "CHECKING";

        case OTA_STATE_DOWNLOADING:
            return "DOWNLOADING";

        case OTA_STATE_VERIFYING:
            return "VERIFYING";

        case OTA_STATE_BACKUP:
            return "BACKUP";

        case OTA_STATE_INSTALLING:
            return "INSTALLING";

        case OTA_STATE_RESTARTING:
            return "RESTARTING";

        case OTA_STATE_SUCCESS:
            return "SUCCESS";

        case OTA_STATE_FAILED:
            return "FAILED";

        case OTA_STATE_ROLLBACK:
            return "ROLLBACK";

        default:
            return "UNKNOWN";
    }
}

/*=============================================================
 *           Read Current Firmware Version
 *============================================================*/

/**
 * @brief Read installed firmware version.
 *
 * Reads version.txt from the application directory.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
 int ota_read_current_version(void)
{
    FILE *fp;

    fp = fopen(OTA_VERSION_FILE, "r");

    if (fp == NULL)
    {
        printf("OTA: Unable to open %s\n",
               OTA_VERSION_FILE);

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
        strcspn(g_ota.info.current_version, "\r\n")
    ] = '\0';

    printf("OTA: Current Version : %s\n",
           g_ota.info.current_version);

    return 0;
}


/*=============================================================
 *                 OTA Initialization
 *============================================================*/

/**
 * @brief Initialize OTA service.
 *
 * Initializes:
 * - OTA context
 * - OTA configuration
 * - Current firmware version
 * - Mosquitto library
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_init(void)
{
    memset(&g_ota, 0, sizeof(g_ota));

    g_ota.state = OTA_STATE_IDLE;

    printf("\n========================================\n");
    printf("        OTA SERVICE INITIALIZATION\n");
    printf("========================================\n");

    /* Load configuration */
    if (ota_load_config() != 0)
    {
        printf("OTA: Failed to load configuration\n");
        return -1;
    }

    /*
     * Store configuration locally.
     *
     * ota_load_config() should return
     * the configuration structure or
     * fill g_ota.config directly depending
     * on your implementation.
     */

    /* Read installed version */
    if (ota_read_current_version() != 0)
    {
        printf("OTA: Failed to read current version\n");
        return -1;
    }

    printf("Current Version : %s\n",
           g_ota.info.current_version);

    mosquitto_lib_init();

    printf("Mosquitto Library Initialized\n");

    printf("OTA Initialization Complete\n\n");

    return 0;
}

/*=============================================================
 *               Start OTA Service
 *============================================================*/

/**
 * @brief Start OTA background service.
 *
 * Creates OTA worker thread.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
int ota_start(void)
{
    if (g_ota.running)
    {
        printf("OTA Service Already Running\n");
        return 0;
    }

    g_ota.running = true;

    if (pthread_create(&g_ota.thread,
                       NULL,
                       ota_thread,
                       NULL) != 0)
    {
        printf("OTA: Thread Creation Failed\n");

        g_ota.running = false;

        return -1;
    }

    printf("OTA Thread Started\n");

    return 0;
}

/*=============================================================
 *                Stop OTA Service
 *============================================================*/

/**
 * @brief Stop OTA service.
 */
void ota_stop(void)
{
    if (!g_ota.running)
    {
        return;
    }

    printf("Stopping OTA Service...\n");

    g_ota.running = false;

    pthread_join(g_ota.thread, NULL);

    ota_mqtt_disconnect();

    mosquitto_lib_cleanup();

    printf("OTA Service Stopped\n");
}

/*=============================================================
 *             Get Current OTA State
 *============================================================*/

/**
 * @brief Return current OTA state.
 *
 * @return OTA state.
 */
ota_state_t ota_get_state(void)
{
    return g_ota.state;
}

/*=============================================================
 *            Update OTA State
 *============================================================*/

/**
 * @brief Change OTA state.
 *
 * @param state New state.
 */
static void ota_set_state(ota_state_t state)
{
    g_ota.state = state;

    printf("OTA State -> %s\n",
           ota_state_string(state));

    ota_publish_status(
        ota_state_string(state));
}

/*=============================================================
 *          Reset OTA Information
 *============================================================*/

/**
 * @brief Reset OTA runtime information.
 */
static void ota_reset_info(void)
{
    memset(&g_ota.info.latest_version,
           0,
           sizeof(g_ota.info.latest_version));

    memset(&g_ota.info.package_name,
           0,
           sizeof(g_ota.info.package_name));

    memset(&g_ota.info.package_url,
           0,
           sizeof(g_ota.info.package_url));

    memset(&g_ota.info.sha256_url,
           0,
           sizeof(g_ota.info.sha256_url));

    memset(&g_ota.info.sha256,
           0,
           sizeof(g_ota.info.sha256));

    memset(&g_ota.info.package_path,
           0,
           sizeof(g_ota.info.package_path));

    memset(&g_ota.info.backup_path,
           0,
           sizeof(g_ota.info.backup_path));

    g_ota.info.update_available = false;
}

/*=============================================================
 *            MQTT Initialization
 *============================================================*/

/**
 * @brief Create MQTT client.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
static int ota_create_client(void)
{
    g_ota.mosq =
        mosquitto_new(NULL,
                      true,
                      &g_ota);

    if (g_ota.mosq == NULL)
    {
        printf("OTA: mosquitto_new() failed\n");
        return -1;
    }

    mosquitto_connect_callback_set(
        g_ota.mosq,
        ota_on_connect);

    mosquitto_disconnect_callback_set(
        g_ota.mosq,
        ota_on_disconnect);

    mosquitto_message_callback_set(
        g_ota.mosq,
        ota_on_message);

    return 0;
}

/*=============================================================
 *                 OTA Worker Thread
 *============================================================*/

/**
 * @brief OTA background thread.
 *
 * Connects to ThingsBoard and waits for OTA commands.
 *
 * @param arg Unused.
 *
 * @return NULL
 */
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

        mosquitto_loop(g_ota.mosq, 1000, 1);

        usleep(100000);
    }

    mosquitto_destroy(g_ota.mosq);
    g_ota.mosq = NULL;

    return NULL;
}

/*=============================================================
 *                 MQTT Connection
 *============================================================*/

/**
 * @brief Connect to ThingsBoard.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
static int ota_mqtt_connect(void)
{
    ota_set_state(OTA_STATE_CONNECTING);

    int rc = mosquitto_username_pw_set(
                    g_ota.mosq,
                    g_ota.config.mqtt_token,
                    NULL);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: Username set failed\n");
        return -1;
    }

    rc = mosquitto_connect(
                g_ota.mosq,
                g_ota.config.mqtt_host,
                g_ota.config.mqtt_port,
                60);

    if (rc != MOSQ_ERR_SUCCESS)
    {
        printf("OTA: Connect failed (%d)\n", rc);
        return -1;
    }

    printf("OTA: Connected to broker\n");

    return 0;
}

/*=============================================================
 *                MQTT Disconnect
 *============================================================*/

/**
 * @brief Disconnect MQTT.
 */
static void ota_mqtt_disconnect(void)
{
    if (g_ota.mosq != NULL)
    {
        mosquitto_disconnect(g_ota.mosq);
    }

    g_ota.mqtt_connected = false;
}

/*=============================================================
 *                 Subscribe RPC Topic
 *============================================================*/

/**
 * @brief Subscribe to ThingsBoard RPC topic.
 *
 * @retval 0 Success
 * @retval -1 Failure
 */
static int ota_mqtt_subscribe(void)
{
    int rc;

    rc = mosquitto_subscribe(
            g_ota.mosq,
            NULL,
            "v1/devices/me/rpc/request/+",
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
 *              MQTT Connect Callback
 *============================================================*/

/**
 * @brief MQTT connected callback.
 */
static void ota_on_connect(struct mosquitto *mosq,
                           void *userdata,
                           int rc)
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

/*=============================================================
 *           MQTT Disconnect Callback
 *============================================================*/

/**
 * @brief MQTT disconnected callback.
 */
static void ota_on_disconnect(struct mosquitto *mosq,
                              void *userdata,
                              int rc)
{
    (void)mosq;
    (void)userdata;
    (void)rc;

    g_ota.mqtt_connected = false;

    printf("OTA: MQTT Disconnected\n");
}

/*=============================================================
 *               MQTT Message Callback
 *============================================================*/

/**
 * @brief Receive OTA RPC command.
 *
 * Expected payload:
 *
 * {
 *     "method":"fw_update"
 * }
 */
static void ota_on_message(struct mosquitto *mosq,
                           void *userdata,
                           const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)userdata;

    if (msg == NULL)
    {
        return;
    }

    printf("\n==============================\n");
    printf("OTA RPC Received\n");
    printf("Topic   : %s\n", msg->topic);
    printf("Payload : %s\n", (char *)msg->payload);
    printf("==============================\n");

    if (strstr((char *)msg->payload, "fw_update") == NULL)
    {
        return;
    }

    printf("OTA Update Requested\n");

    if (ota_execute_update() != 0)
    {
        printf("OTA Update Failed\n");
    }
    else
    {
        printf("OTA Update Completed\n");
    }
}

int ota_compare_version(const char *current,
                        const char *latest)
{
    int c_major, c_minor, c_patch;
    int l_major, l_minor, l_patch;

    if (sscanf(current, "%d.%d.%d",
               &c_major,
               &c_minor,
               &c_patch) != 3)
    {
        return -1;
    }

    if (sscanf(latest, "%d.%d.%d",
               &l_major,
               &l_minor,
               &l_patch) != 3)
    {
        return -1;
    }

    if (l_major > c_major)
        return 1;

    if (l_major < c_major)
        return -1;

    if (l_minor > c_minor)
        return 1;

    if (l_minor < c_minor)
        return -1;

    if (l_patch > c_patch)
        return 1;

    if (l_patch < c_patch)
        return -1;

    return 0;
}

int ota_execute_update(void)
{
    if (ota_download_latest_json() != 0)
        return -1;

    if (ota_parse_latest_json() != 0)
        return -1;

    ota_context_t *ctx = ota_get_context();

    if (!ctx->info.update_available)
    {
        printf("OTA: Already running latest version.\n");
        return 0;
    }

    if (ota_download_package() != 0)
        return -1;

    if (ota_download_sha256() != 0)
        return -1;

    if (ota_verify_package() != 0)
        return -1;

    if (ota_install_package() != 0)
        return -1;

    return 0;
}