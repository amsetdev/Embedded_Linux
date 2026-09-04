/**
 * @file ota.c
 * @brief Over-The-Air update implementation.
 *
 * Handles dual OTA updates (application binary and system image)
 * triggered via MQTT commands. Downloads files using libcurl,
 * verifies integrity with SHA256 (OpenSSL), and applies updates.
 */

#include "ota.h"
#include "settings.h"
#include "mqtt.h"
#include "https.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <openssl/sha.h>

/* -------------------------------------------------------------------------- */
/* External state                                                             */
/* -------------------------------------------------------------------------- */

extern volatile int running;

/* -------------------------------------------------------------------------- */
/* Internal state                                                             */
/* -------------------------------------------------------------------------- */

/** @brief Mutex protecting the OTA request. */
static pthread_mutex_t ota_mutex = PTHREAD_MUTEX_INITIALIZER;

/** @brief Condition variable signalling a new OTA request. */
static pthread_cond_t ota_cond = PTHREAD_COND_INITIALIZER;

/** @brief Shared OTA request. */
static ota_request_t ota_req;

/* -------------------------------------------------------------------------- */
/* Status strings                                                             */
/* -------------------------------------------------------------------------- */

/** @brief Human-readable status names for JSON reports. */
static const char *status_str[] = {
    "STARTED",
    "DOWNLOADING",
    "VERIFYING",
    "APPLYING",
    "SUCCEEDED",
    "FAILED"
};

/** @brief Human-readable type names for JSON reports. */
static const char *type_str[] = {
    "app",
    "system"
};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Reports OTA status to the MQTT status topic.
 *
 * @param type    Update type.
 * @param status  Current status.
 * @param version Target version string.
 * @param error   Error description (empty string if none).
 */
static void ota_report_status(ota_type_t type,
                              ota_status_t status,
                              const char *version,
                              const char *error)
{
    char payload[512];

    snprintf(payload, sizeof(payload),
             "{\"type\":\"%s\",\"status\":\"%s\","
             "\"version\":\"%s\",\"previous_version\":\"%s\","
             "\"error\":\"%s\",\"timestamp\":%lld}",
             type_str[type],
             status_str[status],
             version ? version : "",
             cfg.app_version,
             error ? error : "",
             (long long)time(NULL));

    mqtt_publish_to(cfg.ota_status_topic, payload, 1);

    printf("[OTA] Status: %s %s v%s %s\n",
           type_str[type],
           status_str[status],
           version ? version : "?",
           error ? error : "");
}

/**
 * @brief Computes SHA256 of a file and compares to expected hex.
 *
 * @param filepath     Path to the file.
 * @param expected_hex Expected SHA256 as a 64-character hex string.
 *
 * @return 1 if match, 0 if mismatch or error.
 */
static int verify_sha256(const char *filepath, const char *expected_hex)
{
    FILE *fp = fopen(filepath, "rb");

    if (!fp)
    {
        perror("[OTA] verify_sha256 fopen");
        return 0;
    }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    unsigned char buf[8192];
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        SHA256_Update(&ctx, buf, n);
    }

    fclose(fp);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    /* Convert to hex string. */
    char hex[65];

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }

    hex[64] = '\0';

    if (strcasecmp(hex, expected_hex) != 0)
    {
        fprintf(stderr, "[OTA] SHA256 mismatch: expected %s, got %s\n",
                expected_hex, hex);
        return 0;
    }

    printf("[OTA] SHA256 verified: %s\n", hex);

    return 1;
}

/**
 * @brief Creates a directory path recursively (mkdir -p equivalent).
 *
 * @param path Directory path to create.
 *
 * @return 0 on success, -1 on failure.
 */
static int mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }

    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* -------------------------------------------------------------------------- */
/* App OTA                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief Processes an application OTA update.
 *
 * Downloads the new binary, verifies SHA256 if provided, backs up
 * the current binary, replaces it, and restarts the application.
 *
 * @param req Pointer to the OTA request.
 */
static void ota_process_app(const ota_request_t *req)
{
    char dl_path[512];

    snprintf(dl_path, sizeof(dl_path),
             "%s/main.new", cfg.ota_download_dir);

    /* ---- Download ---- */

    ota_report_status(OTA_TYPE_APP, OTA_STATUS_DOWNLOADING,
                      req->version, "");

    if (!https_download_file(req->url, dl_path, 300))
    {
        ota_report_status(OTA_TYPE_APP, OTA_STATUS_FAILED,
                          req->version, "download_failed");
        return;
    }

    /* ---- Verify SHA256 ---- */

    if (req->sha256[0] != '\0')
    {
        ota_report_status(OTA_TYPE_APP, OTA_STATUS_VERIFYING,
                          req->version, "");

        if (!verify_sha256(dl_path, req->sha256))
        {
            unlink(dl_path);
            ota_report_status(OTA_TYPE_APP, OTA_STATUS_FAILED,
                              req->version, "sha256_mismatch");
            return;
        }
    }

    /* ---- Apply ---- */

    ota_report_status(OTA_TYPE_APP, OTA_STATUS_APPLYING,
                      req->version, "");

    /* Backup current binary. */
    char bak_path[512];
    snprintf(bak_path, sizeof(bak_path),
             "%s.bak", cfg.app_binary_path);

    if (rename(cfg.app_binary_path, bak_path) != 0 && errno != ENOENT)
    {
        fprintf(stderr, "[OTA] Backup failed: %s\n", strerror(errno));
        unlink(dl_path);
        ota_report_status(OTA_TYPE_APP, OTA_STATUS_FAILED,
                          req->version, "backup_failed");
        return;
    }

    /* Move new binary into place. */
    if (rename(dl_path, cfg.app_binary_path) != 0)
    {
        fprintf(stderr, "[OTA] Replace failed: %s\n", strerror(errno));

        /* Restore backup. */
        rename(bak_path, cfg.app_binary_path);

        ota_report_status(OTA_TYPE_APP, OTA_STATUS_FAILED,
                          req->version, "replace_failed");
        return;
    }

    /* Set executable permission. */
    chmod(cfg.app_binary_path, 0755);

    ota_report_status(OTA_TYPE_APP, OTA_STATUS_SUCCEEDED,
                      req->version, "");

    printf("[OTA] App update applied. Restarting...\n");

    /* Flush filesystem buffers. */
    sync();

    /* Allow MQTT status message to be sent. */
    sleep(1);

    /*
     * Restart the application.
     * execv() replaces the process with the new binary.
     * If execv fails, fall back to exit(0) for systemd Restart=always.
     */
    char *argv[] = { cfg.app_binary_path, NULL };
    execv(cfg.app_binary_path, argv);

    /* execv only returns on error. */
    perror("[OTA] execv");
    exit(0);
}

/* -------------------------------------------------------------------------- */
/* System OTA                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief Processes a system OTA update.
 *
 * Downloads the .swu image and invokes swupdate to apply it.
 *
 * @param req Pointer to the OTA request.
 */
static void ota_process_system(const ota_request_t *req)
{
    char dl_path[512];

    snprintf(dl_path, sizeof(dl_path),
             "%s/update.swu", cfg.ota_download_dir);

    /* ---- Download ---- */

    ota_report_status(OTA_TYPE_SYSTEM, OTA_STATUS_DOWNLOADING,
                      req->version, "");

    if (!https_download_file(req->url, dl_path, 600))
    {
        ota_report_status(OTA_TYPE_SYSTEM, OTA_STATUS_FAILED,
                          req->version, "download_failed");
        return;
    }

    /* ---- Apply via swupdate ---- */

    ota_report_status(OTA_TYPE_SYSTEM, OTA_STATUS_APPLYING,
                      req->version, "");

    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "swupdate -i '%s' -e stable,copy2", dl_path);

    int rc = system(cmd);

    /* Clean up downloaded image. */
    unlink(dl_path);

    if (rc == 0)
    {
        ota_report_status(OTA_TYPE_SYSTEM, OTA_STATUS_SUCCEEDED,
                          req->version, "");

        printf("[OTA] System update applied. Rebooting in 3s...\n");

        sleep(3);

        system("reboot");
    }
    else
    {
        char err[64];
        snprintf(err, sizeof(err), "swupdate_exit_%d", WEXITSTATUS(rc));

        ota_report_status(OTA_TYPE_SYSTEM, OTA_STATUS_FAILED,
                          req->version, err);
    }
}

/* -------------------------------------------------------------------------- */
/* OTA thread                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * @brief OTA worker thread main loop.
 *
 * Waits for OTA requests via condition variable and processes
 * them sequentially. Checks the running flag every 2 seconds.
 *
 * @param arg Unused.
 * @return Always NULL.
 */
void *ota_thread_func(void *arg)
{
    (void)arg;

    printf("[OTA] Thread started\n");

    while (running)
    {
        pthread_mutex_lock(&ota_mutex);

        /* Wait for a request or timeout every 2 seconds. */
        if (!ota_req.pending)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;

            pthread_cond_timedwait(&ota_cond, &ota_mutex, &ts);
        }

        if (!ota_req.pending)
        {
            pthread_mutex_unlock(&ota_mutex);
            continue;
        }

        /* Copy request and clear pending flag after processing. */
        ota_request_t req = ota_req;

        pthread_mutex_unlock(&ota_mutex);

        /* Report start. */
        ota_report_status(req.type, OTA_STATUS_STARTED,
                          req.version, "");

        /* Process the request. */
        if (req.type == OTA_TYPE_APP)
        {
            ota_process_app(&req);
        }
        else
        {
            ota_process_system(&req);
        }

        /* Clear pending flag. */
        pthread_mutex_lock(&ota_mutex);
        ota_req.pending = 0;
        pthread_mutex_unlock(&ota_mutex);
    }

    printf("[OTA] Thread stopped\n");

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* MQTT message callback                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief MQTT message callback for OTA command topics.
 *
 * Parses the incoming JSON message, determines the OTA type from
 * the topic, and queues the request for the OTA thread.
 *
 * @param m   Mosquitto instance.
 * @param ud  User data (unused).
 * @param msg Received message.
 */
void ota_on_message(struct mosquitto *m,
                    void *ud,
                    const struct mosquitto_message *msg)
{
    (void)m;
    (void)ud;

    if (!msg || !msg->topic || !msg->payload || msg->payloadlen == 0)
        return;

    /* Determine OTA type from topic. */
    ota_type_t type;

    if (strcmp(msg->topic, cfg.ota_app_topic) == 0)
    {
        type = OTA_TYPE_APP;
    }
    else if (strcmp(msg->topic, cfg.ota_system_topic) == 0)
    {
        type = OTA_TYPE_SYSTEM;
    }
    else
    {
        /* Not an OTA topic — ignore. */
        return;
    }

    /* Reject if an update is already in progress. */
    pthread_mutex_lock(&ota_mutex);

    if (ota_req.pending)
    {
        pthread_mutex_unlock(&ota_mutex);
        printf("[OTA] Update already in progress — rejecting request\n");
        return;
    }

    /* Parse JSON payload. */
    const char *json = (const char *)msg->payload;

    memset(&ota_req, 0, sizeof(ota_req));
    ota_req.type = type;

    json_get_string(json, "url",
                    ota_req.url, sizeof(ota_req.url));

    json_get_string(json, "version",
                    ota_req.version, sizeof(ota_req.version));

    if (type == OTA_TYPE_APP)
    {
        json_get_string(json, "sha256",
                        ota_req.sha256, sizeof(ota_req.sha256));
    }

    /* Validate required fields. */
    if (ota_req.url[0] == '\0')
    {
        pthread_mutex_unlock(&ota_mutex);
        printf("[OTA] Missing URL in OTA command — ignoring\n");
        return;
    }

    ota_req.pending = 1;

    pthread_cond_signal(&ota_cond);
    pthread_mutex_unlock(&ota_mutex);

    printf("[OTA] Queued %s update v%s\n",
           type_str[type], ota_req.version);
}

/* -------------------------------------------------------------------------- */
/* Init / Cleanup                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the OTA subsystem.
 *
 * Creates the download directory.
 *
 * @return 1 on success, 0 on failure.
 */
int ota_init(void)
{
    memset(&ota_req, 0, sizeof(ota_req));

    if (mkdir_p(cfg.ota_download_dir) != 0)
    {
        fprintf(stderr, "[OTA] Failed to create download dir: %s\n",
                cfg.ota_download_dir);
        return 0;
    }

    printf("[OTA] Initialized (dir: %s)\n", cfg.ota_download_dir);

    return 1;
}

/**
 * @brief Cleans up the OTA subsystem.
 *
 * Clears any pending request and signals the condition variable
 * so the OTA thread can exit cleanly.
 */
void ota_cleanup(void)
{
    pthread_mutex_lock(&ota_mutex);

    ota_req.pending = 0;

    pthread_cond_signal(&ota_cond);
    pthread_mutex_unlock(&ota_mutex);

    printf("[OTA] Cleanup complete\n");
}
