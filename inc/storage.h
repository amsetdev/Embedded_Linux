#ifndef STORAGE_H
#define STORAGE_H

/**
 * offline.h — SQLite-backed offline message store
 *
 * Messages that cannot be published immediately (MQTT offline) are
 * written to a local SQLite database so they can be re-published
 * once connectivity is restored.
 */

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

#endif /* OFFLINE_H */
