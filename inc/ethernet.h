#ifndef ETHERNET_H
#define ETHERNET_H

#include <stddef.h>

#define ETH_INTERFACE "end0"

int ethernet_init(void);

int ethernet_is_connected(void);

int ethernet_has_ip(void);

int ethernet_get_ip(char *ip, size_t len);

#endif