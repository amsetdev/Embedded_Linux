#include "ethernet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARRIER_FILE "/sys/class/net/end0/carrier"

/**********************************************************/

int ethernet_init(void)
{
    printf("[ ETH ] Monitoring interface %s\n", ETH_INTERFACE);
    return 0;
}

/**********************************************************/

int ethernet_is_connected(void)
{
    FILE *fp;
    int carrier = 0;

    fp = fopen(CARRIER_FILE, "r");

    if(fp == NULL)
    {
        perror("[ ETH ] carrier");
        return 0;
    }

    fscanf(fp, "%d", &carrier);

    fclose(fp);

    return carrier;
}

/**********************************************************/

int ethernet_has_ip(void)
{
    FILE *fp;

    char line[256];

    fp = popen("ip -4 addr show " ETH_INTERFACE, "r");

    if(fp == NULL)
        return 0;

    while(fgets(line, sizeof(line), fp))
    {
        if(strstr(line, "inet "))
        {
            pclose(fp);
            return 1;
        }
    }

    pclose(fp);

    return 0;
}

/**********************************************************/

int ethernet_get_ip(char *ip, size_t len)
{
    FILE *fp;

    char line[256];

    fp = popen("ip -4 addr show " ETH_INTERFACE, "r");

    if(fp == NULL)
        return -1;

    while(fgets(line, sizeof(line), fp))
    {
        char *p = strstr(line, "inet ");

        if(p)
        {
            sscanf(p, "inet %63[^/]", ip);

            pclose(fp);

            return 0;
        }
    }

    pclose(fp);

    return -1;
}