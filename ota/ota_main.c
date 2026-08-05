#include "ota.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

static volatile int keep_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    keep_running = 0;
}

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (ota_init() != 0)
    {
        printf("OTA: init failed, exiting\n");
        return 1;
    }

    if (ota_start() != 0)
    {
        printf("OTA: start failed, exiting\n");
        return 1;
    }

    while (keep_running)
    {
        sleep(1);
    }

    ota_stop();
    return 0;
}