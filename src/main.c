#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include "config.h"
#include "display.h"
#include "touch.h"
#include "modbus_rtu.h"
#include "mqtt.h"

static void handle_signal(int sig) { (void)sig; running = 0; }

int main(void)
{
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* ---- Settings ---- */
    settings_load();

    printf("\n=== MODBUS RTU READER — STM32MP157F-DK2 ===\n");
    printf("  Port:     %s @ %d  Slave: %d\n",
           cfg.modbus_port, cfg.modbus_baud, cfg.modbus_slave);
    printf("  MQTT:     %s:%d\n", cfg.mqtt_broker, cfg.mqtt_port);
    printf("  Interval: %ds\n\n", cfg.interval);

    /* ---- Display ---- */
    if (drm_init() == 0) {
        disp_ok = 1;
        fb_fill(COL_BLACK);
        drm_flush();
    } else {
        LOG_WARN("Display disabled — running headless");
    }

 
    if (disp_ok) {
        fb_fill(COL_BLUE);
        fb_rect(0, 0, DISP_W, 40, COL_HDRBLUE);
        fb_str_c(10,  "AMSET",                COL_GOLD,  COL_HDRBLUE, 3);
        fb_str_c(60,  "Modbus RTU Reader",    COL_WHITE, COL_BLUE,    2);
        fb_str_c(90,  "STM32MP157F-DK2",      COL_GRAY,  COL_BLUE,    1);
        fb_str_c(110, "HiveMQ Cloud MQTT",    COL_GRAY,  COL_BLUE,    1);
        fb_hline(20, 130, DISP_W - 40, COL_GOLD);
        fb_str_c(140, "Starting...",          COL_GOLD,  COL_BLUE,    1);
        drm_flush();
        sleep(2);
    }


    touch_init();

 
    if (!parse_csv()) return 1;


    if (!mb_connect()) {
        if (disp_ok) {
            fb_fill(COL_BLUE);
            fb_str_c(DISP_H / 2 - 10, "Modbus Connect FAILED", COL_RED,  COL_BLUE, 1);
            fb_str_c(DISP_H / 2 +  6, cfg.modbus_port,        COL_GRAY, COL_BLUE, 1);
            drm_flush();
        }
        return 1;
    }


    db_init();
    mqtt_init();


    pthread_t mb_thread;
    pthread_create(&mb_thread, NULL, mb_thread_func, NULL);
    LOG_INFO("Modbus background thread started");

    while (running) {
        touch_poll();

        if (cur_screen == SCREEN_SETTINGS) {
            disp_settings();
        } else {
            int cyc, ok, succ;
            pthread_mutex_lock(&points_mutex);
            cyc  = mb_cycle;
            ok   = mb_ok_flag;
            succ = mb_success_cnt;
            pthread_mutex_unlock(&points_mutex);
            disp_status(cyc, ok, (int)mqtt_connected, succ, point_count);
        }

        usleep(100000);   /* 100 ms → ~10 fps */
    }

    /* ---- Shutdown ---- */
    LOG_INFO("Shutting down...");
    pthread_join(mb_thread, NULL);

    if (disp_ok) { fb_fill(COL_BLACK); drm_flush(); }

    mb_disconnect();
    mqtt_cleanup();
    touch_close();
    drm_cleanup();

    printf("\n=== STOPPED ===\n");
    return 0;
}