#include "mqtt.h"
#include "settings.h"
#include "data.h"
#include "storage.h"
#include "connection.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mosquitto.h>


/*

also need a internet connection flag 
if LAN is down so stop try to connect mqtt broker 
check every second internet_connection_flag 
if flag is 1 try to connect mqtt brokar 
else in loop check inetrnet flage


*/ 
volatile int mqtt_connected = 0;
static struct mosquitto *mosq = NULL;

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud;
    mqtt_connected = (rc == 0);
    if (rc == 0){
    printf("[ MQTT ] Connected\n");
     
     
        //if (main_thrade if null so start the upload data thrade in mqtt after connect the mqtt  )
     } else if(!(int)mqtt_connected){
            offline_replay_start();
       // offline_replay_start();
       //if mqtt connect so check the sd_card offline store logs if datam is present upload the data
       // offline_replay_start();
     }else{
        fprintf(stderr, "[MQTT] Connection failed (code %d)\n", rc);
     }
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud; (void)rc;
    mqtt_connected = 0;
    printf("[MQTT] Disconnected\n");
}

void reconnect_mqtt(void){
     mqtt_cleanup();
     mqtt_init();
 //mosquitto_disconnect_callback_set(mosq, on_disconnect);

}

int mqtt_init(void)
{
    mosquitto_lib_init();
    char cid[64];
    snprintf(cid, sizeof(cid), "modbus_%ld", (long)time(NULL));

    mosq = mosquitto_new(cid, true, NULL);
    if (!mosq) return 0;

    mosquitto_username_pw_set(mosq, cfg.mqtt_user, cfg.mqtt_pass);
    mosquitto_tls_set(mosq,
        "/etc/ssl/certs/ca-certificates.crt", NULL, NULL, NULL, NULL);
    mosquitto_tls_opts_set(mosq, 1, NULL, NULL);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_disconnect_callback_set(mosq, on_disconnect);

    if (mosquitto_connect(mosq, cfg.mqtt_broker,
                          cfg.mqtt_port, 60) != MOSQ_ERR_SUCCESS)
        return 0;

    mosquitto_loop_start(mosq);
    sleep(2);  /* allow time for the connection handshake */
    return 1;
}


void build_payload(char *buf, size_t buflen)
{
    long long ts = (long long)time(NULL) * 1000;
    int pos = snprintf(buf, buflen, "{\"ts\":%lld,\"values\":{", ts);
    int first = 1;

    ModbusPoint *points = data_get_points();
    int count           = data_get_count();

    for (int i = 0; i < count && pos < (int)buflen - 128; i++) {
        if (!points[i].valid) continue;
        if (!first) buf[pos++] = ',';
        pos += snprintf(buf+pos, buflen-pos,
                        "\"%s\":%d", points[i].label, points[i].value);
        first = 0;
    }
    snprintf(buf+pos, buflen-pos, "}}");
}

void mqtt_publish(const char *payload)
{
    if (mqtt_connected && internet_up) {
        if (mosquitto_publish(mosq, NULL, MQTT_TOPIC,
                (int)strlen(payload), payload, 1, false) == MOSQ_ERR_SUCCESS) {
            printf("[MQTT] Published %zu bytes\n", strlen(payload));
            return;
        }
    }
    /* Not connected or publish failed — save for later */
    offline_store(payload);
}


void mqtt_cleanup(void)
{
    if (mosq) {
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        mosq = NULL;
    }
}
