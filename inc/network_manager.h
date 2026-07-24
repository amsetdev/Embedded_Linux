#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

void network_init(void);
void network_stop(void);
int network_is_online(void);
void network_monitor(void);

#endif