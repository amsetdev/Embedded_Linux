#ifndef STORAGE_H
#define STORAGE_H


#define MQTT_STORAGE_DB "mqtt_storage.db"

int offline_init(void);

void offline_store(const char *payload);

void offline_cleanup(void);

int  offline_init(void);
void offline_store(const char *payload);
void offline_replay_start(void);
void offline_cleanup(void);  

#endif /* OFFLINE_H */
