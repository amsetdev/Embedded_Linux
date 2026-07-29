*/
    *@file connection.c
         *@brief Internet connectivity monitoring and MQTT reconnection module.
             *
                 *This module periodically checks internet connectivity by attempting a TCP
                     *connection to a known public DNS server.It maintains the global internet
                         *connectivity status and automatically attempts to reconnect the MQTT client
                             *when internet access is restored.
                                 *Features : *-Periodic internet connectivity monitoring.*
    -Interruptible monitoring thread using condition variables.* -Automatic MQTT reconnection after connectivity recovery.* -Graceful startup and shutdown of the monitoring thread.* /

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "modbus.h"
#include "display.h"
#include "settings.h"
#include "data.h"
#include "mqtt.h"
#include "storage.h"
#include "mb_tcp.h"
#include "drive_logger.h"
#include "connection.h"

/** @brief Public DNS server used for internet connectivity check. */
#define CHECK_HOST "8.8.8.8"

/** @brief TCP port used for connectivity verification (DNS). */
#define CHECK_PORT 53

/** @brief Interval (in seconds) between connectivity checks. */
#define CHECK_INTERVAL 5

/** @brief Socket connection timeout (in seconds). */
#define CHECK_TIMEOUT_S 2

    /** @brief Global internet connectivity status (1 = connected, 0 = disconnected). */
    volatile int internet_up = 0;

/** @brief Thread handle for the connectivity monitoring thread. */
static pthread_t conn_tid;

/** @brief Flag indicating whether the connectivity monitoring thread is running. */
static volatile int conn_running = 0;

/** @brief Mutex protecting condition variable operations. */
static pthread_mutex_t cv_mutex = PTHREAD_MUTEX_INITIALIZER;

/** @brief Condition variable used to interrupt thread sleep during shutdown. */
static pthread_cond_t cv_stop = PTHREAD_COND_INITIALIZER;

/**
 * @brief Checks whether internet connectivity is available.
 *
 * Attempts to establish a TCP connection to Google's public DNS server
 * (8.8.8.8:53). A successful connection indicates internet availability.
 *
 * @return int
 * @retval 1 Internet connection is available.
 * @retval 0 Internet connection is unavailable.
 */
static int check_internet(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return 0;

    struct timeval tv;
    tv.tv_sec = CHECK_TIMEOUT_S;
    tv.tv_usec = 0;

    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CHECK_PORT);
    inet_pton(AF_INET, CHECK_HOST, &addr.sin_addr);

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);

    return (rc == 0) ? 1 : 0;
}

/**
 * @brief Sleeps for a specified duration but can be interrupted.
 *
 * Waits using a timed condition variable instead of sleep(), allowing the
 * monitoring thread to wake immediately when a shutdown signal is received.
 *
 * @param seconds Number of seconds to wait.
 */
static void interruptible_sleep(int seconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;

    pthread_mutex_lock(&cv_mutex);

    while (conn_running)
    {
        int rc = pthread_cond_timedwait(&cv_stop, &cv_mutex, &deadline);

        if (rc == 0)
            break;

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);

        if (now.tv_sec >= deadline.tv_sec)
            break;
    }

    pthread_mutex_unlock(&cv_mutex);
}

/**
 * @brief Connectivity monitoring thread.
 *
 * Periodically checks internet connectivity and updates the global
 * internet_up flag. If internet connectivity is restored and the MQTT client
 * is disconnected, it attempts to reconnect the MQTT client.
 *
 * @param arg Unused thread argument.
 *
 * @return Always returns NULL.
 */
static void *connection_thread_fn(void *arg)
{
    (void)arg;

    printf("[ Conn ] Connectivity monitor started.------------------------------------------\n");

    while (conn_running)
    {
        int up = check_internet();

        if (up != internet_up)
        {
            printf("[Conn] Internet %s\n", up ? "UP" : "DOWN");

            /*
            if (internet_up)
            {
                printf("[ Connection ] reconnect mqtt \n");
                reconnect_mqtt();
            }
            */
        }

        internet_up = up;

        interruptible_sleep(CHECK_INTERVAL);

        if (internet_up && !mqtt_connected)
        {
            printf("[ Connection ] reconnect mqtt \n");
            reconnect_mqtt();
        }
    }

    printf("[Conn] Connectivity monitor stopped.\n");
    return NULL;
}

/**
 * @brief Initializes the connectivity monitoring service.
 *
 * Creates and starts the connectivity monitoring thread if it is not already
 * running.
 */
void connection_init(void)
{
    if (conn_running)
        return;

    conn_running = 1;

    if (pthread_create(&conn_tid, NULL, connection_thread_fn, NULL) != 0)
    {
        perror("[Conn] pthread_create");
        conn_running = 0;
    }
}

/**
 * @brief Stops the connectivity monitoring service.
 *
 * Signals the monitoring thread to terminate, wakes it if it is sleeping,
 * and waits for the thread to exit before returning.
 */
void connection_stop(void)
{
    if (!conn_running)
        return;

    pthread_mutex_lock(&cv_mutex);

    conn_running = 0;
    pthread_cond_signal(&cv_stop);

    pthread_mutex_unlock(&cv_mutex);

    pthread_join(conn_tid, NULL);
}