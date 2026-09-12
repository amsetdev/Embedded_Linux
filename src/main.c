/**
 * @file main.c
 * @brief Smart RTU main application.
 *
 * This application:
 * - Loads application configuration from smart_rtu_config.json.
 * - Applies Wi-Fi configuration from the generated JSON file.
 * - Reads Modbus RTU devices over RS485 via the fieldbus abstraction.
 * - Reads Modbus TCP devices (when enabled in config).
 * - Publishes collected data to AWS IoT Core via MQTT (X.509 mTLS).
 * - Stores data locally when required.
 * - Synchronizes the RTC using NTP.
 * - Updates the display and touch interface.
 * - Handles graceful shutdown.
 * - Supports SIGHUP for configuration hot-reload.
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
#include <string.h>
#include <stdatomic.h>

#include "fieldbus.h"
#include "display.h"
#include "settings.h"
#include "data.h"
#include "mqtt.h"
#include "connection.h"
#include "https.h"
#include "storage.h"
#include "mb_tcp.h"
#include "drive_logger.h"
#include "rtc.h"
#include "wifi.h"
#include "ota.h"
#include "watchdog.h"
#include "msg_queue.h"

/* -------------------------------------------------------------------------- */
/* Global application state                                                   */
/* -------------------------------------------------------------------------- */

/**
 * @brief Application run flag.
 *
 * Set to zero by SIGINT/SIGTERM to stop all worker threads.
 */
atomic_int running = 1;

/**
 * @brief Configuration reload flag.
 *
 * Set to 1 by SIGHUP to trigger hot-reload of settings and MQTT.
 */
static volatile sig_atomic_t reload_requested = 0;

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
 * @brief Indicates whether the Modbus RTU thread was started.
 */
static int mb_rtu_started = 0;

/**
 * @brief Indicates whether the Modbus TCP thread was started.
 */
static int mb_tcp_started = 0;

/**
 * @brief Indicates whether the RTC sync thread was started.
 */
static int rtc_started = 0;

/**
 * @brief RTC synchronization thread handle.
 */
static pthread_t rtc_thread_id;

/**
 * @brief OTA update thread handle.
 */
static pthread_t ota_thread_id;

/**
 * @brief Indicates whether the OTA thread was started.
 */
static int ota_started = 0;

/**
 * @brief Watchdog monitor thread handle.
 */
static pthread_t wdg_thread_id;

/**
 * @brief Indicates whether the watchdog thread was started.
 */
static int wdg_started = 0;

/**
 * @brief Watchdog heartbeat ID for the Modbus RTU thread.
 */
static int wdg_id_rtu = -1;

/**
 * @brief Watchdog heartbeat ID for the RTC sync thread.
 */
static int wdg_id_rtc = -1;

/**
 * @brief Watchdog heartbeat ID for the Modbus TCP thread.
 */
static int wdg_id_tcp = -1;

/**
 * @brief Watchdog heartbeat ID for the OTA thread.
 */
static int wdg_id_ota = -1;

/**
 * @brief Watchdog heartbeat ID for the MQTT publisher thread.
 */
static int wdg_id_pub = -1;

/**
 * @brief MQTT publisher thread handle.
 */
static pthread_t pub_thread_id;

/**
 * @brief Global message queue (RTU producer -> MQTT publisher consumer).
 */
static msg_queue_t *publish_queue = NULL;

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

/**
 * @brief SIGHUP handler for configuration hot-reload.
 *
 * Sets a flag that is checked in the main loop. The actual
 * reload is performed in the main thread context.
 *
 * @param sig Received signal number.
 */
static void handle_sighup(int sig)
{
    (void)sig;

    reload_requested = 1;
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
        watchdog_heartbeat(wdg_id_rtc);

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
 * builds the MQTT payload and enqueues it for the publisher
 * thread.
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

        mb_ok_flag = 1;
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
        /* Enqueue payload for publisher thread                          */
        /* -------------------------------------------------------------- */

        msg_queue_push(publish_queue, payload);

        /* -------------------------------------------------------------- */
        /* Report watchdog heartbeat                                     */
        /* -------------------------------------------------------------- */

        watchdog_heartbeat(wdg_id_rtu);

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
/* MQTT publisher thread                                                      */
/* -------------------------------------------------------------------------- */

/**
 * @brief MQTT publisher thread.
 *
 * Consumes messages from the publish queue and sends them via
 * MQTT and HTTPS. Runs independently from the Modbus polling
 * thread so network latency does not stall data acquisition.
 *
 * @param arg Unused.
 *
 * @return Always NULL.
 */
static void *mqtt_publisher_thread_func(void *arg)
{
    (void)arg;

    printf("[PUB] Publisher thread started\n");

    while (running)
    {
        char *payload = msg_queue_pop(publish_queue);

        if (!payload)
        {
            /* Queue shut down or empty on exit. */
            break;
        }

        /* ---- Publish via MQTT ---- */

        printf("[PUB] Publishing payload\n");

        mqtt_publish(payload);

        /* ---- HTTPS test endpoint ---- */

        https_post("https://httpbin.org/post", payload);

        free(payload);

        /* ---- Report watchdog heartbeat ---- */

        watchdog_heartbeat(wdg_id_pub);
    }

    printf("[PUB] Publisher thread stopped\n");

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
    signal(SIGHUP, handle_sighup);

    /* ------------------------------------------------------------------ */
    /* Start drive logger                                                */
    /* ------------------------------------------------------------------ */

    drive_logger_start();

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

    printf("  MQTT     : %s:%d (client: %s)\n",
           cfg.mqtt_broker,
           cfg.mqtt_port,
           cfg.mqtt_client_id);

    printf("  MQTT TLS : CA=%s\n",
           cfg.mqtt_ca_cert);

    printf("             Cert=%s\n",
           cfg.mqtt_device_cert);

    printf("             Key=%s\n",
           cfg.mqtt_private_key);

    printf("  Topic    : %s\n",
           cfg.mqtt_topic);

    printf("  RS485 DE : PE9 (UART7 hardware RTS, kernel RS485 mode)\n");

    printf("  Interval : %ds\n\n",
           cfg.interval);

    /* ------------------------------------------------------------------ */
    /* Initialize Modbus RTU via fieldbus abstraction                     */
    /* ------------------------------------------------------------------ */

    fieldbus_config_t rtu_cfg;
    memset(&rtu_cfg, 0, sizeof(rtu_cfg));

    strncpy(rtu_cfg.protocol, "modbus_rtu", sizeof(rtu_cfg.protocol) - 1);
    strncpy(rtu_cfg.serial_port, cfg.modbus_port, sizeof(rtu_cfg.serial_port) - 1);
    rtu_cfg.baud      = cfg.modbus_baud;
    rtu_cfg.slave_id  = cfg.modbus_slave;
    strncpy(rtu_cfg.parity, cfg.modbus_parity, sizeof(rtu_cfg.parity) - 1);
    rtu_cfg.stop_bits = cfg.modbus_stop_bits;

    int rtu_ok = data_init_driver(fieldbus_get_modbus_rtu(), &rtu_cfg);

    if (!rtu_ok)
    {
        fprintf(stderr,
                "[WARN] Modbus RTU driver init failed — "
                "RTU polling disabled\n");
    }

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
                 "AWS IoT Core MQTT",
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

    if (rtu_ok && !parse_registers())
    {
        fprintf(stderr,
                "[WARN] No register configuration — "
                "RTU polling disabled\n");
        rtu_ok = 0;
    }

    /* ------------------------------------------------------------------ */
    /* Initialize MQTT and HTTP                                          */
    /* ------------------------------------------------------------------ */

    mqtt_init();

    /*
     * Starts the background thread that maintains internet_up (checked by
     * mqtt_publish() before every publish) and triggers MQTT reconnection
     * once connectivity returns.
     */
    connection_init();

    http_init();

    /* ------------------------------------------------------------------ */
    /* Create message queue and start publisher thread                    */
    /* ------------------------------------------------------------------ */

    publish_queue = msg_queue_create(0);

    if (!publish_queue)
    {
        fprintf(stderr,
                "[ERROR] Failed to create message queue\n");

        mqtt_cleanup();
        http_cleanup();

        data_close_driver();

        if (disp_ok)
            drm_cleanup();

        drive_logger_stop();

        return 1;
    }

    if (pthread_create(&pub_thread_id,
                       NULL,
                       mqtt_publisher_thread_func,
                       NULL) != 0)
    {
        perror("[ERROR] Failed to create publisher thread");

        msg_queue_destroy(publish_queue);

        mqtt_cleanup();
        http_cleanup();

        data_close_driver();

        if (disp_ok)
            drm_cleanup();

        drive_logger_stop();

        return 1;
    }

    printf("[PUB] MQTT publisher thread started\n");

    /* ------------------------------------------------------------------ */
    /* Start OTA thread (if enabled)                                      */
    /* ------------------------------------------------------------------ */

    if (cfg.ota_enable)
    {
        if (ota_init())
        {
            if (pthread_create(&ota_thread_id,
                               NULL,
                               ota_thread_func,
                               NULL) == 0)
            {
                ota_started = 1;
                printf("[OTA] OTA thread started\n");
            }
            else
            {
                perror("[WARN] Failed to create OTA thread");
            }
        }
    }
    else
    {
        printf("[OTA] OTA disabled by configuration\n");
    }

    /* ------------------------------------------------------------------ */
    /* Initialize watchdog and register threads (if enabled)             */
    /* ------------------------------------------------------------------ */

    if (cfg.watchdog_enable)
    {
        if (watchdog_init())
        {
            wdg_id_rtu = watchdog_register("modbus_rtu");
            wdg_id_pub = watchdog_register("mqtt_publisher");
            wdg_id_rtc = watchdog_register("rtc_sync");

            if (cfg.modbus_tcp_enable)
                wdg_id_tcp = watchdog_register("modbus_tcp");

            if (cfg.ota_enable)
            {
                wdg_id_ota = watchdog_register("ota");
                ota_set_wdg_id(wdg_id_ota);
            }

            if (pthread_create(&wdg_thread_id,
                               NULL,
                               watchdog_thread_func,
                               NULL) == 0)
            {
                wdg_started = 1;
                printf("[WDG] Monitor thread started\n");
            }
            else
            {
                perror("[WARN] Failed to create watchdog thread");
            }
        }
    }
    else
    {
        printf("[WDG] Watchdog disabled by configuration\n");
    }

    /* ------------------------------------------------------------------ */
    /* Start Modbus RTU polling thread                                    */
    /* ------------------------------------------------------------------ */

    if (rtu_ok)
    {
        if (pthread_create(&mb_thread_id,
                           NULL,
                           mb_thread_func,
                           NULL) != 0)
        {
            perror("[WARN] Failed to create Modbus RTU thread");
        }
        else
        {
            mb_rtu_started = 1;
            printf("[MB] Modbus RTU thread started\n");
        }
    }
    else
    {
        printf("[MB] Modbus RTU polling skipped (driver/config not available)\n");
    }

    /* ------------------------------------------------------------------ */
    /* Start Modbus TCP polling thread (if enabled)                      */
    /* ------------------------------------------------------------------ */

    if (cfg.modbus_tcp_enable && cfg.modbus_tcp_ip[0] != '\0')
    {
        mb_thread_arg_t mb_arg;
        memset(&mb_arg, 0, sizeof(mb_arg));

        strncpy(mb_arg.slave_ip,
                cfg.modbus_tcp_ip,
                sizeof(mb_arg.slave_ip) - 1);

        mb_arg.slave_port = cfg.modbus_tcp_port;
        mb_arg.slave_id   = cfg.modbus_tcp_slave_id;
        mb_arg.wdg_id     = wdg_id_tcp;

        if (pthread_create(&mb_tcp_thread_id,
                           NULL,
                           mb_thread_func1,
                           &mb_arg) != 0)
        {
            perror("[WARN] Failed to create Modbus TCP thread");
        }
        else
        {
            mb_tcp_started = 1;

            printf("[MB] Modbus TCP thread started (%s:%d slave %d)\n",
                   cfg.modbus_tcp_ip,
                   cfg.modbus_tcp_port,
                   cfg.modbus_tcp_slave_id);
        }
    }
    else
    {
        printf("[MB] Modbus TCP disabled by configuration\n");
    }

    /* ------------------------------------------------------------------ */
    /* Start RTC synchronization thread                                  */
    /* ------------------------------------------------------------------ */

    if (pthread_create(&rtc_thread_id,
                       NULL,
                       rtc_sync_thread_func,
                       NULL) != 0)
    {
        perror("[WARN] Failed to create RTC thread");
    }
    else
    {
        rtc_started = 1;
        printf("[RTC] Sync thread started\n");
    }

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
        /* -------------------------------------------------------------- */
        /* SIGHUP hot-reload                                             */
        /* -------------------------------------------------------------- */

        if (reload_requested)
        {
            reload_requested = 0;

            printf("[MAIN] SIGHUP received -- reloading configuration\n");

            settings_reload();

            mqtt_cleanup();
            mqtt_init();

            printf("[MAIN] Config reloaded: broker=%s client=%s topic=%s\n",
                   cfg.mqtt_broker,
                   cfg.mqtt_client_id,
                   cfg.mqtt_topic);
        }

        /* -------------------------------------------------------------- */
        /* Display update (skip when running headless)                   */
        /* -------------------------------------------------------------- */

        if (disp_ok)
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
                            (int)atomic_load(&mqtt_connected),
                            succ,
                            data_get_count());
            }
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

    if (mb_rtu_started)
        pthread_join(mb_thread_id, NULL);

    /* Shut down the publish queue so the publisher thread exits. */
    msg_queue_shutdown(publish_queue);
    pthread_join(pub_thread_id, NULL);

    if (mb_tcp_started)
        pthread_join(mb_tcp_thread_id, NULL);

    if (rtc_started)
        pthread_join(rtc_thread_id, NULL);

    if (ota_started)
    {
        ota_cleanup();
        pthread_join(ota_thread_id, NULL);
    }

    if (wdg_started)
    {
        pthread_join(wdg_thread_id, NULL);
        watchdog_cleanup();
    }

    /* ------------------------------------------------------------------ */
    /* Message queue cleanup                                             */
    /* ------------------------------------------------------------------ */

    msg_queue_destroy(publish_queue);
    publish_queue = NULL;

    /* ------------------------------------------------------------------ */
    /* Fieldbus driver cleanup                                           */
    /* ------------------------------------------------------------------ */

    data_close_driver();

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
    /* Connectivity monitor cleanup                                       */
    /* ------------------------------------------------------------------ */

    connection_stop();

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
