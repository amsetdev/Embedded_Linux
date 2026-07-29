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

#define STORAGE_DIR          "/home/root/edb_c/linking/storage"
#define MAX_PATH             256
#define REPLAY_INTERVAL_SEC  60

/** @brief Replay thread handle. */
static pthread_t replay_thread;

/** @brief Indicates whether the replay thread is running. */
static volatile int replay_running = 0;

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
    if (mkdir(STORAGE_DIR, 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr,
                "[SD_CARD] mkdir %s: %s\n",
                STORAGE_DIR,
                strerror(errno));
        return 0;
    }

    printf("[SD_CARD] Storage dir ready: %s\n", STORAGE_DIR);

    return 1;
}

/**
 * @brief Stores an MQTT payload for later transmission.
 *
 * The payload is written to a text file using the current
 * timestamp as the filename.
 *
 * @param payload Null-terminated MQTT payload.
 */
void offline_store(const char *payload)
{
    if (!payload)
        return;

    long long ms = (long long)time(NULL) * 1000;

    char path[MAX_PATH];

    snprintf(path,
             sizeof(path),
             "%s/%lld.txt",
             STORAGE_DIR,
             ms);

    FILE *f = fopen(path, "w");

    if (!f)
    {
        fprintf(stderr,
                "[SD_CARD] fopen %s: %s\n",
                path,
                strerror(errno));
        return;
    }

    fprintf(f, "%s", payload);

    fclose(f);

    printf("[SD_CARD] Stored → %s\n", path);
}

/**
 * @brief Offline replay worker thread.
 *
 * Periodically scans the storage directory for pending payloads.
 * When MQTT and internet connectivity are available, the oldest
 * stored payload is published and removed from storage.
 *
 * @param arg Unused thread argument.
 *
 * @return Always returns NULL.
 */
static void *replay_worker(void *arg)
{
    (void)arg;

    while (replay_running)
    {
        sleep(REPLAY_INTERVAL_SEC);

        if (!mqtt_connected || !internet_up)
        {
            printf("[SD_CARD] Not connected - replay skipped\n");
            continue;
        }

        DIR *dir = opendir(STORAGE_DIR);

        if (!dir)
        {
            fprintf(stderr,
                    "[SD_CARD] opendir: %s\n",
                    strerror(errno));
            continue;
        }

        char oldest_name[MAX_PATH] = {0};
        struct dirent *entry;

        while ((entry = readdir(dir)) != NULL)
        {
            const char *dot = strrchr(entry->d_name, '.');

            if (!dot || strcmp(dot, ".txt") != 0)
                continue;

            if (oldest_name[0] == '\0' ||
                strcmp(entry->d_name, oldest_name) < 0)
            {
                strncpy(oldest_name,
                        entry->d_name,
                        sizeof(oldest_name) - 1);
            }
        }

        closedir(dir);

        if (oldest_name[0] == '\0')
        {
            printf("[SD_CARD] No pending files\n");
            continue;
        }

        char path[MAX_PATH];

        snprintf(path,
                 sizeof(path),
                 "%s/%s",
                 STORAGE_DIR,
                 oldest_name);

        FILE *f = fopen(path, "r");

        if (!f)
        {
            fprintf(stderr,
                    "[SD_CARD] fopen %s: %s\n",
                    path,
                    strerror(errno));
            continue;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);

        char *buf = malloc(sz + 1);

        if (!buf)
        {
            fclose(f);
            continue;
        }

        if (fread(buf, 1, sz, f) != (size_t)sz)
        {
            fprintf(stderr,
                    "[SD_CARD] fread short read: %s\n",
                    oldest_name);

            free(buf);
            fclose(f);
            continue;
        }

        buf[sz] = '\0';

        fclose(f);

        printf("[SD_CARD] Uploading %s\n", oldest_name);

        mqtt_publish(buf);

        free(buf);

        if (remove(path) != 0)
        {
            fprintf(stderr,
                    "[SD_CARD] remove %s: %s\n",
                    path,
                    strerror(errno));
        }
        else
        {
            printf("[SD_CARD] Deleted %s\n", oldest_name);
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
    if (replay_running)
        return;

    replay_running = 1;

    if (pthread_create(&replay_thread,
                       NULL,
                       replay_worker,
                       NULL) != 0)
    {
        fprintf(stderr,
                "[SD_CARD] pthread_create: %s\n",
                strerror(errno));

        replay_running = 0;
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
    if (!replay_running)
        return;

    replay_running = 0;

    pthread_join(replay_thread, NULL);

    printf("[SD_CARD] Replay thread stopped\n");
}