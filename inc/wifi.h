#ifndef WIFI_H
#define WIFI_H

#include <stddef.h>
#include "settings.h"

int wifi_connect(void);
int wifi_is_connected(void);
int wifi_has_ip(void);
int wifi_get_ip(char *ip, size_t len);
void wifi_disconnect(void);

#endif