/**
 * @file data.h
 * @brief Slave-side register snapshot, configuration, and simulation tick.
 *
 * The master version of this module polled external devices and
 * stored results into a local point table. As a SLAVE, there is
 * nothing to poll — the roles are reversed: external masters poll
 * *this* device. This module now:
 *   - owns the periodic "tick" that advances the demo/simulated
 *     values in the shared register map (mb_regmap.h), and
 *   - exposes a read-only ModbusPoint-shaped snapshot of the
 *     holding registers, purely so display.c and mqtt.c (which
 *     show/publish "current values") need no logic changes beyond
 *     reading from the slave's own exposed data instead of a
 *     polled-from-elsewhere value.
 */

#ifndef DATA_H
#define DATA_H

#include "modbus.h"
#include "mb_regmap.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of points in the display/MQTT snapshot. */
#define MAX_POINTS     MB_NUM_HOLDING_REGS

/** @brief Maximum register label length. */
#define LABEL_MAX      64

/** @brief Maximum engineering unit string length. */
#define UNIT_MAX       16

/**
 * @brief Snapshot of one exposed holding register, for display/MQTT.
 */
typedef struct
{
    char    label[LABEL_MAX];
    int     address;
    RegType reg_type;
    char    data_type;
    char    unit[UNIT_MAX];
    int     value;
    int     valid;
} ModbusPoint;

/**
 * @brief Advances the shared register map's demo/simulated values by
 *        one step. Call periodically (e.g. once per second) from a
 *        background thread.
 */
void data_tick(void);

/**
 * @brief Number of simulation ticks performed since startup.
 *
 * @return Tick count.
 */
unsigned long data_get_tick_count(void);

/**
 * @brief Returns a read-only snapshot of the currently exposed
 *        holding registers (label "HR<addr>", current value, valid=1),
 *        refreshed from the shared register map on every call.
 *
 * @return Pointer to an internal, statically-allocated array of
 *         ::data_get_count() entries. Valid until the next call.
 */
ModbusPoint *data_get_points(void);

/**
 * @brief Number of entries returned by data_get_points().
 */
int data_get_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DATA_H */
