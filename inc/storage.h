#ifndef STORAGE_H
#define STORAGE_H


#define MQTT_STORAGE_DB "mqtt_storage.db"

/* ---- API --------------------------------------------------------------- */

/** Open (or create) the SQLite database and the messages table.
 *  Returns 1 on success, 0 on failure. */
int offline_init(void);

/** Store payload for later delivery.
 *  Thread-safe (internal mutex). */
void offline_store(const char *payload);

/** Close the database handle. */
void offline_cleanup(void);

int  offline_init(void);
void offline_store(const char *payload);
void offline_replay_start(void);   /* ← new */
void offline_cleanup(void);        /* now also joins replay thread */

#endif /* OFFLINE_H */
