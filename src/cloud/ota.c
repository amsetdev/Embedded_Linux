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
#include "watchdog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <openssl/sha.h>

/* -------------------------------------------------------------------------- */
/* External state                                                             */
/* -------------------------------------------------------------------------- */

extern atomic_int running;

/** @brief Watchdog heartbeat ID for the OTA thread (-1 if disabled). */
static int ota_wdg_id = -1;

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

    ota_status_json(payload, sizeof(payload), type, status,
                    version, cfg.app_version, error, (long long)time(NULL));

    mqtt_publish_to(cfg.ota_status_topic, payload, 1);

    printf("[OTA] Status: %s %s v%s %s\n",
           ota_type_name(type),
           ota_status_name(status),
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

    char hex[65];

    if (!ota_digest_matches(hash, expected_hex, hex))
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
 * @brief ota_app_ops_t.download: libcurl download (https_download_file()).
 * @param ctx Unused.
 * @param url Source URL.
 * @param path Destination file.
 * @param timeout_sec Transfer timeout.
 * @return 1 on success, 0 on failure.
 */
static int app_download(void *ctx, const char *url, const char *path, long timeout_sec)
{
    (void)ctx;
    return https_download_file(url, path, timeout_sec);
}

/**
 * @brief ota_app_ops_t.verify: verify_sha256().
 * @param ctx Unused.
 * @param path Downloaded file.
 * @param expected_hex Expected SHA256 (hex).
 * @return 1 if it matches, 0 otherwise.
 */
static int app_verify(void *ctx, const char *path, const char *expected_hex)
{
    (void)ctx;
    return verify_sha256(path, expected_hex);
}

/**
 * @brief ota_app_ops_t.rename: rename(2).
 * @param ctx Unused.
 * @param from Existing path.
 * @param to New path.
 * @return 0, or the errno value.
 */
static int app_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    return rename(from, to) == 0 ? 0 : errno;
}

/**
 * @brief ota_app_ops_t.unlink: unlink(2).
 * @param ctx Unused.
 * @param path File to delete.
 */
static void app_unlink(void *ctx, const char *path)
{
    (void)ctx;
    unlink(path);
}

/**
 * @brief ota_app_ops_t.make_executable: chmod 0755.
 * @param ctx Unused.
 * @param path File.
 */
static void app_make_executable(void *ctx, const char *path)
{
    (void)ctx;
    chmod(path, 0755);
}

/**
 * @brief ota_app_ops_t.report: publishes the status of an application update.
 * @param ctx Unused.
 * @param status Status to report.
 * @param version Target version.
 * @param error Error code ("" if none).
 */
static void app_report(void *ctx, ota_status_t status, const char *version, const char *error)
{
    (void)ctx;
    ota_report_status(OTA_TYPE_APP, status, version, error);
}

/** @brief The real side effects of an application update. */
static const ota_app_ops_t app_ops = {
    .ctx             = NULL,
    .download        = app_download,
    .verify          = app_verify,
    .rename          = app_rename,
    .unlink          = app_unlink,
    .make_executable = app_make_executable,
    .report          = app_report,
};

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
    if (!ota_apply_app(&app_ops, req, cfg.ota_download_dir, cfg.app_binary_path))
        return;

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

/*
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
        watchdog_heartbeat(ota_wdg_id);

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

/*
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

    ota_request_t req;

    ota_parse_result_t parsed = ota_parse_request(msg->topic, (const char *)msg->payload,
                                                  cfg.ota_app_topic, cfg.ota_system_topic,
                                                  &req);
    if (parsed == OTA_REQ_NOT_OTA)
        return;   /* Not an OTA topic — ignore. */

    /* Rejected request (bad URL, missing/invalid sha256): tell the cloud why. */
    const char *reject = ota_parse_error(parsed);

    if (reject)
    {
        printf("[OTA] Rejected %s update v%s: %s\n",
               ota_type_name(req.type), req.version, reject);
        ota_report_status(req.type, OTA_STATUS_FAILED, req.version, reject);
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

    /* Validate required fields. */
    if (parsed == OTA_REQ_MISSING_URL)
    {
        pthread_mutex_unlock(&ota_mutex);
        printf("[OTA] Missing URL in OTA command — ignoring\n");
        return;
    }

    ota_req = req;
    ota_req.pending = 1;

    pthread_cond_signal(&ota_cond);
    pthread_mutex_unlock(&ota_mutex);

    printf("[OTA] Queued %s update v%s\n",
           ota_type_name(req.type), req.version);
}

/* -------------------------------------------------------------------------- */
/* Init / Cleanup                                                             */
/* -------------------------------------------------------------------------- */

/*
 * @brief Sets the watchdog heartbeat ID for the OTA thread.
 *
 * @param id  Watchdog ID from watchdog_register().
 */
void ota_set_wdg_id(int id)
{
    ota_wdg_id = id;
}

/**
 * @brief Initializes the OTA subsystem.
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
