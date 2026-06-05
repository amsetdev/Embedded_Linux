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
#include "storage.h"

volatile int     running        = 1;
static int       mb_cycle       = 0;
static int       mb_ok_flag     = 0;
static int       mb_success_cnt = 0;
static pthread_mutex_t points_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mb_thread_id;

static void handle_signal(int sig) { (void)sig; running = 0; }


static void *mb_thread_func(void *arg)
{
    (void)arg;
    static char payload[PAYLOAD_MAX];

    while (running) {
        /* Read all configured Modbus points */
        read_all_points();
        printf("modbus address can read form excel and use ");

        int s = 0;
        ModbusPoint *pts = data_get_points();
        int cnt          = data_get_count();
        for (int i = 0; i < cnt; i++)
            if (pts[i].valid) s++;

        pthread_mutex_lock(&points_mutex);
        mb_ok_flag     = (uart_fd >= 0);    
        mb_success_cnt = s;
        mb_cycle++;
        pthread_mutex_unlock(&points_mutex);

        printf("[MB] Cycle %d done — %d/%d ok\n", mb_cycle, s, cnt);

        /* Build JSON and publish (or store offline) */
        printf("bulding json payload ");
        build_payload(payload, sizeof(payload));
        // mqtt_publish(payload);
        printf("publishing json :");
        mqtt_publish(payload);

        /* Wait cfg.interval seconds before next cycle (interruptible) */
        // time interval for next modbus cycle
        for (int t = 0; t < cfg.interval * 10 && running; t++)
            usleep(100000);
    }
    return NULL;
}


int main(void)
{

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    //if user can change software setting then startup can import mqtt,time interval configration form setting.config
    printf("load the setting form setting.config");
    settings_load();

    printf("\n=== MODBUS RTU READER — STM32MP157F-DK2 ===\n");
    printf("  Port     : %s @ %d  Slave: %d\n",
           cfg.modbus_port, cfg.modbus_baud, cfg.modbus_slave);
    printf("  MQTT     : %s:%d\n", cfg.mqtt_broker, cfg.mqtt_port);
    printf("  RS485 DE : PE10 (gpiochip4 line 10)\n");
    printf("  DE fix   : write() -> tcdrain() -> guard(%dus) -> DE LOW -> read()\n",
           RS485_TX_GUARD_US);
    printf("  Interval : %ds\n\n", cfg.interval);
    //time interval print 

   
    if (rs485_gpio_init() < 0) //PE10 pin rs485
        fprintf(stderr, "[WARN] RS485 GPIO init failed — DE pin uncontrolled\n");
    rs485_rx();   /* ensure LOW */  
    // low =resive mod
    //high = trasmit mode 

   
    if (uart_open(cfg.modbus_port, cfg.modbus_baud) < 0) {
        fprintf(stderr, "[ERROR] Cannot open %s\n", cfg.modbus_port);
        rs485_gpio_close();
        return 1;
    }
    printf("[UART] Open: %s @ %d baud\n", cfg.modbus_port, cfg.modbus_baud);

  
    if (drm_init() == 0) {
        disp_ok = 1;
        fb_fill(COL_BLACK);
        drm_flush();
    } else {
        fprintf(stderr, "[WARN] Display disabled — running headless\n");
    }

   
    if (disp_ok) {
        fb_fill(COL_BLUE);
        fb_rect(0,0,DISP_W,40,COL_HDRBLUE);
        fb_str_c(10,"AMSET",COL_GOLD,COL_HDRBLUE,3);
        fb_str_c(60,"Modbus RTU Reader",COL_WHITE,COL_BLUE,2);
        fb_str_c(90,"STM32MP157F-DK2",COL_GRAY,COL_BLUE,1);
        fb_str_c(110,"HiveMQ Cloud MQTT",COL_GRAY,COL_BLUE,1);
        fb_hline(20,130,DISP_W-40,COL_GOLD);
        fb_str_c(140,"Starting...",COL_GOLD,COL_BLUE,1);
        drm_flush();
        sleep(2);
    }

    
    touch_init();
    //kernal interface touch handale by kernal 
   
    if (!parse_csv()) {
        uart_close();
        rs485_gpio_close();
        return 1;
    }

   
    offline_init();
    //offline data storage 

   
    mqtt_init();
    //mqtt init connect to mqtt

  
    pthread_create(&mb_thread_id, NULL, mb_thread_func, NULL);
    printf("[MB] Background thread started\n");

   //MB loop
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
            disp_status(cyc, ok, (int)mqtt_connected, succ, data_get_count());
        }
        usleep(100000);   /* ~10 fps UI refresh */
    }

    /* 12. Shutdown */
    printf("\n[MAIN] Shutting down...\n");
    pthread_join(mb_thread_id, NULL);

    rs485_rx();         
    rs485_gpio_close();
    uart_close();

    if (disp_ok) { fb_fill(COL_BLACK); drm_flush(); }

    mqtt_cleanup();
    offline_cleanup();
    drm_cleanup();

    printf("=== STOPPED ===\n");
    return 0;
}
