#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
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

#define CHECK_HOST "8.8.8.8"
#define CHECK_PORT 53
#define CHECK_INTERVAL 5
#define CHECK_TIMEOUT_S 2

volatile int internet_up = 0;

static pthread_t conn_tid;
static volatile int conn_running = 0;

static pthread_mutex_t cv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_stop = PTHREAD_COND_INITIALIZER;

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
            // if(internet_up){
            //     printf("[ Connection ] reconnect mqtt \n");
            //     reconnect_mqtt();
            //     }
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