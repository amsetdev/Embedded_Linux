/**
 * @file main.c
 * @brief Smart RTU main application.
 *
 * This application:
 * - Loads application configuration from smart_rtu_config.json.
 * - Applies Wi-Fi configuration from the generated JSON file.
 * - Reads Modbus RTU devices over RS485.
 * - Reads Modbus TCP devices.
 * - Publishes collected data to MQTT.
 * - Stores data locally when required.
 * - Synchronizes the RTC using NTP.
 * - Updates the display and touch interface.
 * - Handles graceful shutdown.
 *
 * Network management:
 * - Linux/systemd manages Ethernet and Wi-Fi services.
 * - wpa_supplicant manages Wi-Fi association.
 * - DHCP/network services manage IP addresses.
 * - Linux routing metrics determine the preferred interface.
 *
 * The application only:
 * - Applies Wi-Fi configuration.
 * - Reads network status.
 * - Does not manage Ethernet/Wi-Fi routing.
 * - Does not start/stop wpa_supplicant.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

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
#include "wifi.h"

/* -------------------------------------------------------------------------- */
/* Global application state                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Application run flag.
 *
 * Set to zero by SIGINT/SIGTERM to stop all worker threads.
 */
volatile int running = 1;

/**
 * @brief Modbus polling cycle counter.
 */
static int mb_cycle = 0;

/**
 * @brief Modbus communication status flag.
 */
static int mb_ok_flag = 0;

/**
 * @brief Number of successfully read Modbus points.
 */
static int mb_success_cnt = 0;

/**
 * @brief Protects shared Modbus statistics.
 */
static pthread_mutex_t points_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Modbus RTU polling thread handle.
 */
static pthread_t mb_thread_id;

/**
 * @brief Modbus TCP polling thread handle.
 */
static pthread_t mb_tcp_thread_id;

/**
 * @brief RTC synchronization thread handle.
 */
static pthread_t rtc_thread_id;

/* -------------------------------------------------------------------------- */
/* Signal handling                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief Signal handler for graceful shutdown.
 *
 * @param sig Received signal number.
 */
static void handle_signal(int sig)
{
    (void)sig;

    running = 0;
}

/* -------------------------------------------------------------------------- */
/* RTC thread                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief RTC synchronization thread.
 *
 * Attempts RTC synchronization using NTP.
 *
 * Successful synchronization:
 *     retry after 24 hours.
 *
 * Failed synchronization:
 *     retry after 60 seconds.
 *
 * @param arg Unused.
 *
 * @return Always NULL.
 */
static void *rtc_sync_thread_func(void *arg)
{
    (void)arg;

    while (running)
    {
        int result = rtc_main();

        printf("[RTC] Sync result: %d\n", result);

        int wait_sec;

        if (result == 0)
            wait_sec = 86400;
        else
            wait_sec = 60;

        for (int t = 0;
             t < wait_sec && running;
             t++)
        {
            sleep(1);
        }
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Modbus RTU thread                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Modbus RTU polling thread.
 *
 * Reads all registers configured in smart_rtu_config.json,
 * builds the MQTT payload and publishes the collected data.
 *
 * @param arg Unused.
 *
 * @return Always NULL.
 */
static void *mb_thread_func(void *arg)
{
    (void)arg;

    static char payload[PAYLOAD_MAX];

    while (running)
    {
        /* -------------------------------------------------------------- */
        /* Read configured Modbus RTU registers                          */
        /* -------------------------------------------------------------- */

        read_all_points();

        printf("[MODBUS] Reading configured register list\n");

        int success_count = 0;

        ModbusPoint *pts = data_get_points();
        int cnt = data_get_count();

        for (int i = 0; i < cnt; i++)
        {
            if (pts[i].valid)
                success_count++;
        }

        /* -------------------------------------------------------------- */
        /* Update Modbus statistics                                      */
        /* -------------------------------------------------------------- */

        pthread_mutex_lock(&points_mutex);

        mb_ok_flag = (uart_fd >= 0);
        mb_success_cnt = success_count;
        mb_cycle++;

        pthread_mutex_unlock(&points_mutex);

        printf("[MB] Cycle %d complete (%d/%d successful)\n",
               mb_cycle,
               success_count,
               cnt);

        /* -------------------------------------------------------------- */
        /* Build MQTT payload                                            */
        /* -------------------------------------------------------------- */

        printf("[MQTT] Building JSON payload\n");

        build_payload(payload, sizeof(payload));

        /* -------------------------------------------------------------- */
        /* Publish MQTT payload                                         */
        /* -------------------------------------------------------------- */

        printf("[MQTT] Publishing payload\n");

        mqtt_publish(payload);

        /* -------------------------------------------------------------- */
        /* HTTPS test endpoint                                           */
        /* -------------------------------------------------------------- */

        https_post("https://httpbin.org/post", payload);

        /* -------------------------------------------------------------- */
        /* Wait for next Modbus RTU cycle                                */
        /* -------------------------------------------------------------- */

        for (int t = 0;
             t < cfg.interval * 10 && running;
             t++)
        {
            usleep(100000);
        }
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Main application                                                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief Main application entry point.
 *
 * @return
 * - 0 on normal shutdown.
 * - 1 on initialization failure.
 */
int main(void)
{
    /* ------------------------------------------------------------------ */
    /* Signal handlers                                                    */
    /* ------------------------------------------------------------------ */

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* ------------------------------------------------------------------ */
    /* Start drive logger                                                */
    /* ------------------------------------------------------------------ */

    drive_logger_start();

    /* ------------------------------------------------------------------ */
    /* Modbus TCP configuration                                          */
    /* ------------------------------------------------------------------ */

    static mb_thread_arg_t mb_arg =
        {
            .slave_ip = "192.168.0.101",
            .slave_port = 0,
            .slave_id = 0};

    /* ------------------------------------------------------------------ */
    /* Load application configuration                                    */
    /* ------------------------------------------------------------------ */

    printf("[SETTINGS] Loading configuration\n");

    settings_load();

    /* ------------------------------------------------------------------ */
    /* Wi-Fi configuration                                               */
    /* ------------------------------------------------------------------ */
    /*
     * Wi-Fi settings come from:
     *
     *     smart_rtu_config.json
     *
     * settings_load() has already loaded:
     *
     *     cfg.wifi_enable
     *     cfg.wifi_ssid
     *     cfg.wifi_password
     *     cfg.wifi_country
     *
     * wifi_reconfigure() only updates the wpa_supplicant
     * configuration and reloads the existing service.
     *
     * It does NOT kill or start wpa_supplicant.
     */

    if (cfg.wifi_enable)
    {
        printf("[NET] Applying Wi-Fi configuration...\n");

        if (wifi_reconfigure() == 0)
        {
            printf("[NET] Wi-Fi configuration applied\n");

            printf("[NET] Waiting for Wi-Fi connection...\n");

            if (wifi_wait_for_connection(30) == 0)
            {
                char ip[32];

                if (wifi_get_ip(ip, sizeof(ip)) == 0)
                {
                    printf("[NET] Wi-Fi connected: %s\n", ip);
                }
                else
                {
                    printf("[NET] Wi-Fi connected but IP not available\n");
                }
            }
            else
            {
                /*
                 * Do not terminate the application.
                 *
                 * Linux/systemd/wpa_supplicant may establish the
                 * connection shortly after the application starts.
                 */
                printf("[NET] Wi-Fi not connected within 30 seconds\n");
                printf("[NET] Continuing application startup...\n");
            }
        }
        else
        {
            printf("[NET] Wi-Fi configuration failed\n");
            printf("[NET] Continuing application startup...\n");
        }
    }
    else
    {
        printf("[NET] Wi-Fi disabled by configuration\n");
    }

    /* ------------------------------------------------------------------ */
    /* Print current network status                                      */
    /* ------------------------------------------------------------------ */

    network_print_status();

    /* ------------------------------------------------------------------ */
    /* Application information                                           */
    /* ------------------------------------------------------------------ */

    printf("\n");
    printf("=== MODBUS RTU READER — STM32MP157F-DK2 ===\n");

    printf("  Port     : %s @ %d  Slave: %d\n",
           cfg.modbus_port,
           cfg.modbus_baud,
           cfg.modbus_slave);

    printf("  MQTT     : %s:%d\n",
           cfg.mqtt_broker,
           cfg.mqtt_port);

    printf("  RS485 DE : PE10 (gpiochip4 line 10)\n");

    printf("  DE fix   : write() -> tcdrain() -> "
           "guard(%dus) -> DE LOW -> read()\n",
           RS485_TX_GUARD_US);

    printf("  Interval : %ds\n\n",
           cfg.interval);

    /* ------------------------------------------------------------------ */
    /* Initialize RS485 GPIO                                             */
    /* ------------------------------------------------------------------ */

    if (rs485_gpio_init() < 0)
    {
        fprintf(stderr,
                "[WARN] RS485 GPIO init failed - "
                "DE pin uncontrolled\n");
    }

    /* Put RS485 transceiver into receive mode */

    rs485_rx();

    /* ------------------------------------------------------------------ */
    /* Open Modbus RTU UART                                               */
    /* ------------------------------------------------------------------ */

    if (uart_open(cfg.modbus_port, cfg.modbus_baud) < 0)
    {
        fprintf(stderr,
                "[ERROR] Cannot open %s\n",
                cfg.modbus_port);

        rs485_gpio_close();

        drive_logger_stop();

        return 1;
    }

    printf("[UART] Opened %s @ %d baud\n",
           cfg.modbus_port,
           cfg.modbus_baud);

    /* ------------------------------------------------------------------ */
    /* Initialize display                                                */
    /* ------------------------------------------------------------------ */

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

    /* ------------------------------------------------------------------ */
    /* Display startup screen                                             */
    /* ------------------------------------------------------------------ */

    if (disp_ok)
    {
        fb_fill(COL_BLUE);

        fb_rect(0,
                0,
                DISP_W,
                40,
                COL_HDRBLUE);

        fb_str_c(10,
                 "AMSET",
                 COL_GOLD,
                 COL_HDRBLUE,
                 3);

        fb_str_c(60,
                 "Modbus RTU Reader",
                 COL_WHITE,
                 COL_BLUE,
                 2);

        fb_str_c(90,
                 "STM32MP157F-DK2",
                 COL_GRAY,
                 COL_BLUE,
                 1);

        fb_str_c(110,
                 "HiveMQ Cloud MQTT",
                 COL_GRAY,
                 COL_BLUE,
                 1);

        fb_hline(20,
                 130,
                 DISP_W - 40,
                 COL_GOLD);

        fb_str_c(140,
                 "Starting...",
                 COL_GOLD,
                 COL_BLUE,
                 1);

        drm_flush();

        sleep(2);
    }

    /* ------------------------------------------------------------------ */
    /* Initialize touch interface                                        */
    /* ------------------------------------------------------------------ */

    touch_init();

    /* ------------------------------------------------------------------ */
    /* Load Modbus register configuration                                 */
    /* ------------------------------------------------------------------ */

    if (!parse_registers())
    {
        fprintf(stderr,
                "[ERROR] Failed to load register configuration\n");

        uart_close();
        rs485_gpio_close();

        if (disp_ok)
            drm_cleanup();

        drive_logger_stop();

        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Initialize MQTT and HTTP                                          */
    /* ------------------------------------------------------------------ */

    mqtt_init();

    http_init();

    /* ------------------------------------------------------------------ */
    /* Start Modbus RTU polling thread                                    */
    /* ------------------------------------------------------------------ */

    if (pthread_create(&mb_thread_id,
                       NULL,
                       mb_thread_func,
                       NULL) != 0)
    {
        perror("[ERROR] Failed to create Modbus RTU thread");

        mqtt_cleanup();
        http_cleanup();

        uart_close();
        rs485_gpio_close();

        if (disp_ok)
            drm_cleanup();

        drive_logger_stop();

        return 1;
    }

    printf("[MB] Modbus RTU thread started\n");

    /* ------------------------------------------------------------------ */
    /* Start Modbus TCP polling thread                                   */
    /* ------------------------------------------------------------------ */

    if (pthread_create(&mb_tcp_thread_id,
                       NULL,
                       mb_thread_func1,
                       &mb_arg) != 0)
    {
        perror("[ERROR] Failed to create Modbus TCP thread");

        running = 0;

        pthread_join(mb_thread_id, NULL);

        mqtt_cleanup();
        http_cleanup();

        uart_close();
        rs485_gpio_close();

        if (disp_ok)
            drm_cleanup();

        drive_logger_stop();

        return 1;
    }

    printf("[MB] Modbus TCP thread started\n");

    /* ------------------------------------------------------------------ */
    /* Start RTC synchronization thread                                  */
    /* ------------------------------------------------------------------ */

    if (pthread_create(&rtc_thread_id,
                       NULL,
                       rtc_sync_thread_func,
                       NULL) != 0)
    {
        perror("[ERROR] Failed to create RTC thread");

        running = 0;

        pthread_join(mb_thread_id, NULL);
        pthread_join(mb_tcp_thread_id, NULL);

        mqtt_cleanup();
        http_cleanup();

        uart_close();
        rs485_gpio_close();

        if (disp_ok)
            drm_cleanup();

        drive_logger_stop();

        return 1;
    }

    printf("[RTC] Sync thread started\n");

    /* ------------------------------------------------------------------ */
    /* Offline storage                                                   */
    /* ------------------------------------------------------------------ */

    offline_init();

    offline_replay_start();

    /* ------------------------------------------------------------------ */
    /* Main UI loop                                                       */
    /* ------------------------------------------------------------------ */

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

        /*
         * Network status is intentionally NOT used here to select
         * Ethernet or Wi-Fi.
         *
         * Linux routing metrics decide which interface is used.
         */

        usleep(100000);
    }

    /* ------------------------------------------------------------------ */
    /* Shutdown                                                          */
    /* ------------------------------------------------------------------ */

    printf("\n[MAIN] Shutting down...\n");

    /* Stop worker threads */

    pthread_join(mb_thread_id, NULL);

    pthread_join(mb_tcp_thread_id, NULL);

    pthread_join(rtc_thread_id, NULL);

    /* ------------------------------------------------------------------ */
    /* RS485 cleanup                                                     */
    /* ------------------------------------------------------------------ */

    rs485_rx();

    rs485_gpio_close();

    uart_close();

    /* ------------------------------------------------------------------ */
    /* MQTT / HTTP cleanup                                               */
    /* ------------------------------------------------------------------ */

    mqtt_cleanup();

    http_cleanup();

    /* ------------------------------------------------------------------ */
    /* Display cleanup                                                   */
    /* ------------------------------------------------------------------ */

    if (disp_ok)
        drm_cleanup();

    /* ------------------------------------------------------------------ */
    /* Offline storage cleanup                                           */
    /* ------------------------------------------------------------------ */

    offline_cleanup();

    /* ------------------------------------------------------------------ */
    /* Drive logger cleanup                                              */
    /* ------------------------------------------------------------------ */

    drive_logger_stop();

    printf("=== STOPPED ===\n");

    return 0;
}