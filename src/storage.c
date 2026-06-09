#include "storage.h"
#include "mqtt.h"   /* MQTT_TOPIC */

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sqlite3.h>

static sqlite3         *db       = NULL;
static pthread_mutex_t  db_mutex = PTHREAD_MUTEX_INITIALIZER;

int offline_init(void)
{
    if (sqlite3_open(MQTT_STORAGE_DB, &db) != SQLITE_OK) {
        fprintf(stderr, "[Offline] DB open: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    char *err = NULL;
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS messages("
        "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER,"
        "  topic     TEXT,"
        "  data      TEXT,"
        "  published INTEGER DEFAULT 0);",
        NULL, NULL, &err);  

    if (err) {
        fprintf(stderr, "[Offline] DB init: %s\n", err);
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

void offline_store(const char *payload)
{
    if (!db) return;
    pthread_mutex_lock(&db_mutex);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO messages(timestamp, topic, data) VALUES(?,?,?);",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (long long)time(NULL) * 1000);
        sqlite3_bind_text(st,  2, MQTT_TOPIC, -1, SQLITE_STATIC);
        sqlite3_bind_text(st,  3, payload,    -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
        printf("[Offline] Message stored to DB\n");
    }

    pthread_mutex_unlock(&db_mutex);
}

void offline_cleanup(void)
{
    if (db) { sqlite3_close(db); db = NULL; }
}
