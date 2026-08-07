/**
 * @file wifi_main.c
 * @brief Standalone WiFi/Ethernet connectivity daemon.
 *
 * Runs independently of "main" (gateway.service) so that network
 * interfaces are never brought down when the gateway app restarts
 * (e.g. during an OTA update). Owns wlan0/end0 bring-up and
 * Ethernet-priority failover for the whole board.
 */

#include "network_manager.h"
#include "settings.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    keep_running = 0;
}

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[WIFI-SVC] Loading configuration\n");
    settings_load();

    printf("[WIFI-SVC] Starting network manager\n");
    network_init();

    while (keep_running)
    {
        network_monitor();
        sleep(5);
    }

    network_stop();
    printf("[WIFI-SVC] Stopped\n");
    return 0;
}