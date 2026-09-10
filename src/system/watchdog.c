/**
 * @file watchdog.c
 * @brief Hardware watchdog and thread health monitoring.
 *
 * Opens the Linux hardware watchdog (/dev/watchdog) and monitors
 * registered threads via heartbeat timestamps. If any thread goes
 * stale beyond the configured timeout, the hardware watchdog is
 * no longer petted and the board reboots.
 */

#include "watchdog.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

/* -------------------------------------------------------------------------- */
/* External state                                                             */
/* -------------------------------------------------------------------------- */

extern atomic_int running;

/* -------------------------------------------------------------------------- */
/* Internal types                                                             */
/* -------------------------------------------------------------------------- */

/** @brief Per-thread heartbeat entry. */
typedef struct
{
    char   name[32];        /**< Human-readable thread name.       */
    time_t last_heartbeat;  /**< Last heartbeat timestamp (epoch). */
    int    active;          /**< 1 = slot in use.                  */
} wdg_entry_t;

/* -------------------------------------------------------------------------- */
/* Internal state                                                             */
/* -------------------------------------------------------------------------- */

/** @brief Hardware watchdog file descriptor (-1 if not available). */
static int wdg_fd = -1;

/** @brief Thread heartbeat registry. */
static wdg_entry_t wdg_threads[WDG_MAX_THREADS];

/** @brief Number of registered threads. */
static int wdg_count = 0;

/** @brief Mutex protecting the heartbeat registry. */
static pthread_mutex_t wdg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* -------------------------------------------------------------------------- */
/* Hardware watchdog helpers                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Pets the hardware watchdog by writing a byte.
 */
static void wdg_pet(void)
{
    if (wdg_fd >= 0)
    {
        if (write(wdg_fd, "W", 1) != 1)
        {
            perror("[WDG] Failed to pet watchdog");
        }
    }
}

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Initializes the watchdog subsystem.
 *
 * @return 1 on success, 0 on failure.
 */
int watchdog_init(void)
{
    memset(wdg_threads, 0, sizeof(wdg_threads));
    wdg_count = 0;

    wdg_fd = open("/dev/watchdog", O_WRONLY);

    if (wdg_fd < 0)
    {
        fprintf(stderr,
                "[WDG] Cannot open /dev/watchdog: %s\n",
                strerror(errno));
        fprintf(stderr,
                "[WDG] Running in software-only mode "
                "(no automatic reboot)\n");
    }
    else
    {
        printf("[WDG] Hardware watchdog opened (/dev/watchdog)\n");

        /* Pet immediately so the kernel timer starts fresh. */
        wdg_pet();
    }

    printf("[WDG] Initialized (staleness timeout: %ds)\n",
           cfg.watchdog_timeout);

    return 1;
}

/**
 * @brief Registers a thread for health monitoring.
 *
 * @param name  Human-readable thread name.
 * @return Non-negative ID on success, -1 if full.
 */
int watchdog_register(const char *name)
{
    pthread_mutex_lock(&wdg_mutex);

    if (wdg_count >= WDG_MAX_THREADS)
    {
        pthread_mutex_unlock(&wdg_mutex);

        fprintf(stderr,
                "[WDG] Registry full — cannot register '%s'\n",
                name);

        return -1;
    }

    int id = wdg_count;

    strncpy(wdg_threads[id].name, name,
            sizeof(wdg_threads[id].name) - 1);

    wdg_threads[id].name[sizeof(wdg_threads[id].name) - 1] = '\0';
    wdg_threads[id].last_heartbeat = time(NULL);
    wdg_threads[id].active = 1;

    wdg_count++;

    pthread_mutex_unlock(&wdg_mutex);

    printf("[WDG] Registered thread '%s' (id=%d)\n", name, id);

    return id;
}

/**
 * @brief Reports a heartbeat from a monitored thread.
 *
 * @param id  Thread ID from watchdog_register().
 */
void watchdog_heartbeat(int id)
{
    if (id < 0 || id >= WDG_MAX_THREADS)
        return;

    pthread_mutex_lock(&wdg_mutex);

    if (wdg_threads[id].active)
    {
        wdg_threads[id].last_heartbeat = time(NULL);
    }

    pthread_mutex_unlock(&wdg_mutex);
}

/**
 * @brief Watchdog monitor thread.
 *
 * Checks heartbeat timestamps every WDG_CHECK_INTERVAL seconds.
 * Pets the hardware watchdog only when all threads are healthy.
 *
 * @param arg  Unused.
 * @return Always NULL.
 */
void *watchdog_thread_func(void *arg)
{
    (void)arg;

    printf("[WDG] Monitor thread started\n");

    while (running)
    {
        /* Sleep in 1-second increments to respond to shutdown. */
        for (int t = 0; t < WDG_CHECK_INTERVAL && running; t++)
        {
            sleep(1);
        }

        if (!running)
            break;

        time_t now = time(NULL);
        int all_healthy = 1;

        pthread_mutex_lock(&wdg_mutex);

        for (int i = 0; i < wdg_count; i++)
        {
            if (!wdg_threads[i].active)
                continue;

            int age = (int)(now - wdg_threads[i].last_heartbeat);

            if (age > cfg.watchdog_timeout)
            {
                fprintf(stderr,
                        "[WDG] STALE: thread '%s' — "
                        "no heartbeat for %ds (threshold %ds)\n",
                        wdg_threads[i].name,
                        age,
                        cfg.watchdog_timeout);

                all_healthy = 0;
            }
        }

        pthread_mutex_unlock(&wdg_mutex);

        if (all_healthy)
        {
            wdg_pet();
        }
        else
        {
            fprintf(stderr,
                    "[WDG] NOT petting hardware watchdog — "
                    "stale thread(s) detected\n");
        }
    }

    printf("[WDG] Monitor thread stopped\n");

    return NULL;
}

/**
 * @brief Cleans up the watchdog subsystem.
 *
 * Writes the magic close character 'V' to disable the hardware
 * watchdog on graceful shutdown, then closes the file descriptor.
 */
void watchdog_cleanup(void)
{
    if (wdg_fd >= 0)
    {
        /*
         * Writing 'V' (magic close) tells the kernel watchdog
         * driver to disable the timer on close, preventing
         * a reboot during graceful shutdown.
         */
        if (write(wdg_fd, "V", 1) != 1)
        {
            perror("[WDG] Failed to write magic close");
        }

        close(wdg_fd);
        wdg_fd = -1;

        printf("[WDG] Hardware watchdog disabled (graceful shutdown)\n");
    }

    printf("[WDG] Cleanup complete\n");
}
