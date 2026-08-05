/**
 * @file main.c
 * @brief Smart RTU main application.
 *
 * This application performs the following tasks:
 * - Initializes network connectivity (Ethernet/Wi-Fi).
 * - Reads Modbus RTU slave devices over RS485.
 * - Reads Modbus TCP slave devices.
 * - Publishes collected data to an MQTT broker.
 * - Stores data locally when offline and replays it later.
 * - Synchronizes the RTC using NTP.
 * - Updates the display and touch interface.
 * - Handles graceful shutdown on SIGINT/SIGTERM.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "connection.h"
#include "modbus.h"
#include "display.h"
#include "settings.h"
#include "data.h"
#include "mqtt.h"
#include "https.h"
#include "storage.h"
#include "mb_tcp.h"
#include "drive_logger.h"
#include "rtc.h"
#include "network_manager.h"
#include "ethernet.h"
#include "wifi.h"



/** @brief Application run flag. */
volatile int running = 1;

/** @brief Modbus polling cycle counter. */
static int mb_cycle = 0;

/** @brief Modbus communication status flag. */
static int mb_ok_flag = 0;

/** @brief Number of successfully read Modbus points. */
static int mb_success_cnt = 0;

/** @brief Protects shared Modbus statistics. */
static pthread_mutex_t points_mutex = PTHREAD_MUTEX_INITIALIZER;

/** @brief Modbus RTU polling thread handle. */
static pthread_t mb_thread_id;

/** @brief RTC synchronization thread handle. */
static pthread_t rtc_thread_id;

/**
 * @brief Signal handler for graceful shutdown.
 *
 * Sets the global running flag to zero, causing all worker
 * threads and loops to exit cleanly.
 *
 * @param sig Received signal number.
 */
static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/**
 * @brief RTC synchronization thread.
 *
 * Synchronizes the RTC using NTP. On successful synchronization,
 * the next sync occurs after 24 hours. If synchronization fails,
 * retries occur every 60 seconds.
 *
 * @param arg Unused thread argument.
 *
 * @return Always returns NULL.
 */
static void *rtc_sync_thread_func(void *arg)
{
    (void)arg;

    while (running)
    {
        int result = rtc_main();

        printf("[RTC] Sync result: %d\n", result);

        int wait_sec = (result == 0) ? 86400 : 60;

        for (int t = 0; t < wait_sec && running; t++)
        {
            sleep(1);
        }
    }

    return NULL;
}

/**
 * @brief Modbus RTU polling thread.
 *
 * Periodically reads all configured Modbus points,
 * updates statistics, builds the MQTT payload,
 * and publishes or stores data offline.
 *
 * @param arg Unused thread argument.
 *
 * @return Always returns NULL.
 */
static void *mb_thread_func(void *arg)
{
    (void)arg;

    static char payload[PAYLOAD_MAX];

    while (running)
    {
        /* Read configured Modbus points */
        read_all_points();

        printf("[MODBUS] Reading configured register list\n");

        int success_count = 0;

        ModbusPoint *pts = data_get_points();
        int cnt = data_get_count();

        for (int i = 0; i < cnt; i++)
        {
            if (pts[i].valid)
            {
                success_count++;
            }
        }

        pthread_mutex_lock(&points_mutex);

        mb_ok_flag = (uart_fd >= 0);
        mb_success_cnt = success_count;
        mb_cycle++;

        pthread_mutex_unlock(&points_mutex);

        printf("[MB] Cycle %d complete (%d/%d successful)\n",
               mb_cycle,
               success_count,
               cnt);

        /* Build MQTT payload */
        printf("[MQTT] Building JSON payload\n");

        build_payload(payload, sizeof(payload));

        printf("[MQTT] Publishing payload\n");

        mqtt_publish(payload);

        /* HTTPS test endpoint */
        https_post("https://httpbin.org/post", payload);

        /* Wait for next cycle */
        for (int t = 0; t < cfg.interval * 10 && running; t++)
        {
            usleep(100000);
        }
    }

    return NULL;
}

/**
 * @brief Main application entry point.
 *
 * Initializes all system components and starts worker threads.
 *
 * @return
 * - 0 on successful shutdown.
 * - Non-zero on initialization failure.
 */
int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    drive_logger_start();

    static mb_thread_arg_t mb_arg =
        {
            .slave_ip = "192.168.0.20",
            .slave_port = 0,
            .slave_id = 0,
        };

    printf("[SETTINGS] Loading configuration\n");

    settings_load();

    printf("[NET] Starting Network Manager...\n");

    network_init();

    printf("[NET] Waiting for Internet...\n");

    while (!network_is_online())
    {
        printf("[NET] Internet not available...\n");
        sleep(1);
    }

    printf("[NET] Internet Connected\n");

    printf("\n=== MODBUS RTU READER — STM32MP157F-DK2 ===\n");
    printf("  Port     : %s @ %d  Slave: %d\n",
           cfg.modbus_port,
           cfg.modbus_baud,
           cfg.modbus_slave);

    printf("  MQTT     : %s:%d\n",
           cfg.mqtt_broker,
           cfg.mqtt_port);

    printf("  RS485 DE : PE10 (gpiochip4 line 10)\n");

    printf("  DE fix   : write() -> tcdrain() -> guard(%dus) -> DE LOW -> read()\n",
           RS485_TX_GUARD_US);

    printf("  Interval : %ds\n\n",
           cfg.interval);

    if (rs485_gpio_init() < 0)
    {
        fprintf(stderr,
                "[WARN] RS485 GPIO init failed - DE pin uncontrolled\n");
    }

    rs485_rx();

    if (uart_open(cfg.modbus_port, cfg.modbus_baud) < 0)
    {
        fprintf(stderr,
                "[ERROR] Cannot open %s\n",
                cfg.modbus_port);

        rs485_gpio_close();

        return 1;
    }

    printf("[UART] Opened %s @ %d baud\n",
           cfg.modbus_port,
           cfg.modbus_baud);

    if (drm_init() == 0)
    {
        disp_ok = 1;

        fb_fill(COL_BLACK);
        drm_flush();
    }
    else
    {
        fprintf(stderr,
                "[WARN] Display disabled - running headless\n");
    }

    if (disp_ok)
    {
        fb_fill(COL_BLUE);
        fb_rect(0, 0, DISP_W, 40, COL_HDRBLUE);
        fb_str_c(10, "AMSET", COL_GOLD, COL_HDRBLUE, 3);
        fb_str_c(60, "Modbus RTU Reader", COL_WHITE, COL_BLUE, 2);
        fb_str_c(90, "STM32MP157F-DK2", COL_GRAY, COL_BLUE, 1);
        fb_str_c(110, "HiveMQ Cloud MQTT", COL_GRAY, COL_BLUE, 1);
        fb_hline(20, 130, DISP_W - 40, COL_GOLD);
        fb_str_c(140, "Starting...", COL_GOLD, COL_BLUE, 1);

        drm_flush();

        sleep(2);
    }

    touch_init();

    if (!parse_registers())
    {
        uart_close();
        rs485_gpio_close();
        return 1;
    }

    mqtt_init();
    http_init();


    pthread_create(&mb_thread_id,
                   NULL,
                   mb_thread_func,
                   NULL);

    printf("[MB] Modbus RTU thread started\n");

    pthread_t mb_thread_id2;

    pthread_create(&mb_thread_id2,
                   NULL,
                   mb_thread_func1,
                   &mb_arg);

    printf("[MB] Modbus TCP thread started\n");

    pthread_create(&rtc_thread_id,
                   NULL,
                   rtc_sync_thread_func,
                   NULL);

    printf("[RTC] Sync thread started\n");

    offline_init();
    offline_replay_start();

    while (running)
    {
        touch_poll();

        if (cur_screen == SCREEN_SETTINGS)
        {
            disp_settings();
        }
        else
        {
            int cyc;
            int ok;
            int succ;

            pthread_mutex_lock(&points_mutex);

            cyc = mb_cycle;
            ok = mb_ok_flag;
            succ = mb_success_cnt;

            pthread_mutex_unlock(&points_mutex);

            disp_status(cyc,
                        ok,
                        (int)mqtt_connected,
                        succ,
                        data_get_count());
        }

        usleep(100000);
    }

    printf("\n[MAIN] Shutting down...\n");

    pthread_join(mb_thread_id, NULL);
    pthread_join(mb_thread_id2, NULL);
    pthread_join(rtc_thread_id, NULL);
    
    

    rs485_rx();
    rs485_gpio_close();
    uart_close();

    mqtt_cleanup();
    http_cleanup();
    drm_cleanup();
    offline_cleanup();
   
    

    network_stop();

    drive_logger_stop();

    printf("=== STOPPED ===\n");

    return 0;
}