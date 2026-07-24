#include "network_manager.h"
#include "ethernet.h"
#include "wifi.h"
#include "settings.h"

#include <stdio.h>
#include <unistd.h>

typedef enum
{
    NET_NONE = 0,
    NET_ETHERNET,
    NET_WIFI

} network_type_t;

static network_type_t current_network = NET_NONE;

/************************************************************/

static void use_ethernet(void)
{
    char ip[32];

    if(current_network == NET_ETHERNET)
        return;

    printf("\n");
    printf("=====================================\n");
    printf("[ NET ] Switching to Ethernet\n");

    if(ethernet_get_ip(ip,sizeof(ip))==0)
        printf("[ NET ] Ethernet IP : %s\n",ip);

    current_network = NET_ETHERNET;
}

/************************************************************/

static void use_wifi(void)
{
    char ip[32];

    if(current_network == NET_WIFI)
        return;

    printf("\n");
    printf("=====================================\n");
    printf("[ NET ] Switching to WiFi\n");

    wifi_init();

    if(wifi_connect()==0)
    {
        if(wifi_get_ip(ip,sizeof(ip))==0)
            printf("[ NET ] WiFi IP : %s\n",ip);

        current_network = NET_WIFI;
    }
    else
    {
        printf("[ NET ] WiFi connection failed\n");
        current_network = NET_NONE;
    }
}

/************************************************************/

void network_init(void)
{
    printf("\n");
    printf("=====================================\n");
    printf("[ NET ] Network Manager Start\n");

    ethernet_init();

    if(ethernet_is_connected() &&
       ethernet_has_ip())
    {
        use_ethernet();
        return;
    }

    use_wifi();
}

/************************************************************/

void network_monitor(void)
{
    /* Ethernet always has priority */

    if(ethernet_is_connected() &&
       ethernet_has_ip())
    {
        if(current_network != NET_ETHERNET)
        {
            printf("[ NET ] Ethernet detected\n");

            use_ethernet();
        }

        return;
    }

    /* Ethernet unavailable */

    if(current_network == NET_ETHERNET)
    {
        printf("[ NET ] Ethernet disconnected\n");

        current_network = NET_NONE;
    }

    /* Already on WiFi */

    if(current_network == NET_WIFI)
    {
        if(wifi_is_connected())
            return;

        printf("[ NET ] WiFi Lost\n");

        current_network = NET_NONE;
    }

    /* Try WiFi */

    use_wifi();
}

/************************************************************/

int network_is_online(void)
{
    switch(current_network)
    {
        case NET_ETHERNET:
            return ethernet_has_ip();

        case NET_WIFI:
            return wifi_is_connected();

        default:
            return 0;
    }
}