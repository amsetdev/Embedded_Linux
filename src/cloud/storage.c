/**
 * @file storage.c
 * @brief Offline storage and replay implementation.
 *
 * This module provides offline data storage for MQTT messages when
 * network connectivity is unavailable. Messages are stored as text
 * files and replayed automatically once MQTT and internet
 * connectivity are restored.
 *
 * Features:
 * - Creates the offline storage directory.
 * - Stores MQTT payloads as timestamp-based files.
 * - Uploads stored payloads in chronological order.
 * - Deletes files after successful transmission.
 * - Runs replay in a dedicated background thread.
 */

#include "storage.h"
#include "store_forward.h"
#include "paths.h"
#include "mqtt.h"
#include "connection.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdatomic.h>

/** @brief Directory (inside the data directory, paths.h) holding payloads stored while offline. */
#define STORAGE_SUBDIR       "storage"

/** @brief Size of the path buffers for files in the storage directory. */
#define MAX_PATH             (PATHS_MAX + SF_NAME_MAX)

/**
 * @brief Path of a stored file, or of the storage directory itself.
 *
 * @param name File name, or NULL for the directory.
 * @param buf  Output buffer (MAX_PATH bytes).
 * @return buf.
 */
static char *storage_path(const char *name, char *buf)
{
    char rel[MAX_PATH];
    if (name)
        snprintf(rel, sizeof(rel), "%s/%s", STORAGE_SUBDIR, name);
    else
        snprintf(rel, sizeof(rel), "%s", STORAGE_SUBDIR);
    return paths_data(rel, buf, MAX_PATH);
}

/** @brief Seconds between replay rounds (each sends up to SF_REPLAY_BATCH payloads). */
#define REPLAY_INTERVAL_SEC  60

/** @brief How long a replayed payload waits for the broker's PUBACK before it is kept for later. */
#define REPLAY_PUBACK_TIMEOUT_MS  10000

/** @brief Replay thread handle. */
static pthread_t replay_thread;

/** @brief Indicates whether the replay thread is running. */
static atomic_int replay_running = 0;

/**
 * @brief Initializes offline storage.
 *
 * Creates the storage directory if it does not already exist.
 *
 * @return
 * - 1 on success.
 * - 0 on failure.
 */
int offline_init(void)
{
    char dir[MAX_PATH];
    storage_path(NULL, dir);

    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr,
                "[SD_CARD] mkdir %s: %s\n",
                dir,
                strerror(errno));
        return 0;
    }

    printf("[SD_CARD] Storage dir ready: %s\n", dir);

    return 1;
}

/* -------------------------------------------------------------------------- */
/* sf_ops_t on the storage directory and MQTT                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief sf_ops_t.connected: MQTT connected and internet reachable.
 * @param ctx Unused.
 * @return 1 when online, 0 otherwise.
 */
static int fs_connected(void *ctx)
{
    (void)ctx;
    return atomic_load(&mqtt_connected) && atomic_load(&internet_up);
}

/**
 * @brief sf_ops_t.list: every entry of the storage directory.
 * @param ctx Unused.
 * @param each Called with each entry name.
 * @param each_ctx Passed to each.
 * @return 0, or -1 if the storage directory can't be opened.
 */
static int fs_list(void *ctx, void (*each)(const char *name, void *each_ctx), void *each_ctx)
{
    (void)ctx;
    char dir_path[MAX_PATH];
    DIR *dir = opendir(storage_path(NULL, dir_path));

    if (!dir)
    {
        fprintf(stderr, "[SD_CARD] opendir: %s\n", strerror(errno));
        return -1;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
        each(entry->d_name, each_ctx);

    closedir(dir);
    return 0;
}

/**
 * @brief sf_ops_t.read: whole file from the storage directory.
 * @param ctx Unused.
 * @param name File name in the storage directory.
 * @return malloc()ed NUL-terminated content, NULL on failure.
 */
static char *fs_read(void *ctx, const char *name)
{
    (void)ctx;
    char path[MAX_PATH];
    storage_path(name, path);

    FILE *f = fopen(path, "r");

    if (!f)
    {
        fprintf(stderr, "[SD_CARD] fopen %s: %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char *buf = malloc(sz + 1);

    if (!buf)
    {
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, sz, f) != (size_t)sz)
    {
        fprintf(stderr, "[SD_CARD] fread short read: %s\n", name);
        free(buf);
        fclose(f);
        return NULL;
    }

    buf[sz] = '\0';
    fclose(f);

    printf("[SD_CARD] Uploading %s\n", name);
    return buf;
}

/**
 * @brief sf_ops_t.write: new file in the storage directory.
 * @param ctx Unused.
 * @param name File name in the storage directory.
 * @param payload Content.
 * @return 0 on success, -1 on failure.
 */
static int fs_write(void *ctx, const char *name, const char *payload)
{
    (void)ctx;
    char path[MAX_PATH];
    storage_path(name, path);

    /* "x": fail instead of overwriting an existing file. */
    FILE *f = fopen(path, "wx");

    if (!f)
    {
        fprintf(stderr, "[SD_CARD] fopen %s: %s\n", path, strerror(errno));
        return -1;
    }

    int ok = fputs(payload, f) >= 0;

    if (fclose(f) != 0 || !ok)
    {
        fprintf(stderr, "[SD_CARD] write %s failed\n", path);
        remove(path);
        return -1;
    }

    printf("[SD_CARD] Stored → %s\n", path);
    return 0;
}

/**
 * @brief sf_ops_t.publish: publish and wait for the broker's PUBACK.
 * @param ctx Unused.
 * @param payload Payload to publish.
 * @return 0 when the broker confirmed it, -1 otherwise (the file is kept).
 */
static int fs_publish(void *ctx, const char *payload)
{
    (void)ctx;
    return mqtt_publish_confirmed(payload, REPLAY_PUBACK_TIMEOUT_MS);
}

/**
 * @brief sf_ops_t.remove: delete a file from the storage directory.
 * @param ctx Unused.
 * @param name File name in the storage directory.
 * @return 0 on success, -1 on failure.
 */
static int fs_remove(void *ctx, const char *name)
{
    (void)ctx;
    char path[MAX_PATH];
    storage_path(name, path);

    if (remove(path) != 0)
    {
        fprintf(stderr, "[SD_CARD] remove %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

/** @brief The real storage (the storage directory) and network operations. */
static const sf_ops_t fs_ops = {
    .ctx       = NULL,
    .connected = fs_connected,
    .list      = fs_list,
    .read      = fs_read,
    .write     = fs_write,
    .publish   = fs_publish,
    .remove    = fs_remove,
};

/*
 * @brief Stores an MQTT payload for later transmission.
 *
 * The payload is written to a text file using the current
 * timestamp as the filename.
 *
 * @param payload Null-terminated MQTT payload.
 */
void offline_store(const char *payload)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    char name[SF_NAME_MAX];
    sf_store(&fs_ops, payload, (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, name);
}

/**
 * @brief Offline replay worker thread.
 *
 * Every REPLAY_INTERVAL_SEC, when MQTT and internet connectivity are
 * available, up to SF_REPLAY_BATCH stored payloads are published oldest
 * first; each file is removed once the broker confirmed it (sf_replay()).
 *
 * @param arg Unused thread argument.
 *
 * @return Always returns NULL.
 */
static void *replay_worker(void *arg)
{
    (void)arg;

    while (atomic_load(&replay_running))
    {
        /* Sleep in 1 s steps so a shutdown doesn't wait for the full interval. */
        for (int s = 0; s < REPLAY_INTERVAL_SEC && atomic_load(&replay_running); s++)
            sleep(1);

        if (!atomic_load(&replay_running))
            break;

        char name[SF_NAME_MAX];
        sf_replay_result_t last;

        int sent = sf_replay(&fs_ops, SF_REPLAY_BATCH, &last, name);

        if (sent > 0)
            printf("[SD_CARD] Replayed and deleted %d stored payload(s)\n", sent);

        switch (last)
        {
        case SF_NOT_CONNECTED:
            printf("[SD_CARD] Not connected - replay skipped\n");
            break;
        case SF_NOTHING_PENDING:
            if (sent == 0)
                printf("[SD_CARD] No pending files\n");
            break;
        case SF_PUBLISH_FAILED:
            printf("[SD_CARD] %s not confirmed by the broker - kept for the next round\n", name);
            break;
        case SF_SENT:
            printf("[SD_CARD] Batch limit reached, more files pending\n");
            break;
        case SF_LIST_FAILED:
        case SF_READ_FAILED:
        case SF_SENT_NOT_REMOVED:
            break;   /* already logged by the operation */
        }
    }

    return NULL;
}

/**
 * @brief Starts the offline replay thread.
 *
 * Creates a background thread that periodically checks for
 * stored payloads and uploads them when connectivity is
 * available.
 */
void offline_replay_start(void)
{
    if (atomic_load(&replay_running))
        return;

    atomic_store(&replay_running, 1);

    if (pthread_create(&replay_thread,
                       NULL,
                       replay_worker,
                       NULL) != 0)
    {
        fprintf(stderr,
                "[SD_CARD] pthread_create: %s\n",
                strerror(errno));

        atomic_store(&replay_running, 0);
    }
}

/**
 * @brief Stops the offline replay thread.
 *
 * Signals the replay thread to terminate and waits for it
 * to exit.
 */
void offline_cleanup(void)
{
    if (!atomic_load(&replay_running))
        return;

    atomic_store(&replay_running, 0);

    pthread_join(replay_thread, NULL);

    printf("[SD_CARD] Replay thread stopped\n");
}