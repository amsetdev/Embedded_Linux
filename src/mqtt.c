#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sqlite3.h>
#include <mosquitto.h>
#include "mqtt.h"
#include "config.h"

/* ============================================================================
 * MODULE STATE
 * ========================================================================== */

static struct mosquitto *mosq    = NULL;
static sqlite3          *db      = NULL;
static pthread_mutex_t   db_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * SQLITE OFFLINE STORAGE
 * ========================================================================== */

int db_init(void)
{
    if (sqlite3_open(MQTT_STORAGE_DB, &db) != SQLITE_OK) {
        LOG_ERROR("DB: %s", sqlite3_errmsg(db));
        return 0;
    }

    char *err = NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS messages("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "timestamp INTEGER, topic TEXT, data TEXT,"
        "published INTEGER DEFAULT 0);",
        NULL, NULL, &err);

    if (err) {
        LOG_ERROR("DB init: %s", err);
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

void db_store(const char *payload)
{
    if (!db) return;
    pthread_mutex_lock(&db_mutex);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO messages(timestamp,topic,data) VALUES(?,?,?);",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (long long)time(NULL) * 1000);
        sqlite3_bind_text(st,  2, MQTT_TOPIC, -1, SQLITE_STATIC);
        sqlite3_bind_text(st,  3, payload,    -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    pthread_mutex_unlock(&db_mutex);
}

/* ============================================================================
 * MOSQUITTO CALLBACKS
 * ========================================================================== */

static void on_connect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud;
    mqtt_connected = (rc == 0);
    if (rc == 0) LOG_INFO("MQTT connected");
    else         LOG_ERROR("MQTT failed (code %d)", rc);
}

static void on_disconnect(struct mosquitto *m, void *ud, int rc)
{
    (void)m; (void)ud; (void)rc;
    mqtt_connected = 0;
    LOG_WARN("MQTT disconnected");
}

/* ============================================================================
 * MQTT INIT / PUBLISH / CLEANUP
 * ========================================================================== */

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

    if (mosquitto_connect(mosq, cfg.mqtt_broker, cfg.mqtt_port, 60)
            != MOSQ_ERR_SUCCESS) {
        return 0;
    }

    mosquitto_loop_start(mosq);
    sleep(2);   /* let connect callback fire */
    return 1;
}

void mqtt_publish(const char *payload)
{
    if (mqtt_connected) {
        if (mosquitto_publish(mosq, NULL, MQTT_TOPIC,
                (int)strlen(payload), payload, 1, false) == MOSQ_ERR_SUCCESS) {
            LOG_INFO("Published %zu bytes", strlen(payload));
            return;
        }
    }
    db_store(payload);
}

void mqtt_cleanup(void)
{
    if (mosq) {
        mosquitto_loop_stop(mosq, true);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        mosq = NULL;
    }
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}