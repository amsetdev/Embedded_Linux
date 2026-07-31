/**
 * @file data.c
 * @brief Slave-side periodic simulation tick and display/MQTT snapshot.
 *
 * See data.h. Register values are now served TO external masters
 * rather than polled FROM them, so this module's job is to keep
 * mb_regmap's demo/simulated data moving, and to hand display.c /
 * mqtt.c a simple labeled snapshot of what this slave is currently
 * exposing (so those modules need no logic changes).
 */

#include "data.h"

#include <stdio.h>

static unsigned long tick_count = 0;
static ModbusPoint   snapshot[MAX_POINTS];

void data_tick(void)
{
    mb_regmap_tick();
    tick_count++;
}

unsigned long data_get_tick_count(void)
{
    return tick_count;
}

ModbusPoint *data_get_points(void)
{
    mb_regmap_lock();
    for (int i = 0; i < MAX_POINTS; i++) {
        snprintf(snapshot[i].label, sizeof(snapshot[i].label), "HR%d", i);
        snapshot[i].address  = i;
        snapshot[i].reg_type = REG_HOLDING;
        snapshot[i].data_type = 'w';
        snapshot[i].unit[0]  = '\0';
        snapshot[i].value    = mb_regmap_get_holding(i);
        snapshot[i].valid    = 1;
    }
    mb_regmap_unlock();
    return snapshot;
}

int data_get_count(void)
{
    return MAX_POINTS;
}
