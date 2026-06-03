#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <modbus/modbus.h>
#include "config.h"

/* ============================================================================
 * MODBUS MODULE
 *
 * Handles RTU connection, per-point reads, CSV config loading,
 * JSON payload building, and the background polling thread.
 * ========================================================================== */

extern modbus_t *mb_ctx;

/* Connect/disconnect */
int  mb_connect(void);
void mb_disconnect(void);

/* Read a single point; returns 1 on success */
int  read_point(ModbusPoint *pt);

/* Read all points sequentially */
void read_all_points(void);

/* Build JSON payload from current point values */
void build_payload(char *buf, size_t buflen);

/* Background thread entry — reads points, publishes MQTT, sleeps interval */
void *mb_thread_func(void *arg);

#endif /* MODBUS_RTU_H */