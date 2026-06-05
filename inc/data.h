#ifndef DATA_H
#define DATA_H

/**
 * data.h — ModbusPoint data store, CSV parser, and point-read logic
 *
 * Owns the global points[] array.  All other modules access it via
 * data_get_points() / data_get_count() rather than extern arrays.
 */

#include "modbus.h"  /* RegType */

/* ---- Limits ------------------------------------------------------------ */
#define MAX_POINTS  2000
#define LABEL_MAX   64
#define UNIT_MAX    16
#define CONFIG_FILE "registers.csv"

/* ---- Point structure --------------------------------------------------- */
typedef struct {
    char    label[LABEL_MAX];
    int     address;
    RegType reg_type;
    char    data_type;
    char    unit[UNIT_MAX];
    int     value;
    int     valid;
} ModbusPoint;

/* ---- API --------------------------------------------------------------- */

/** Parse registers.csv and populate the internal points array.
 *  Falls back to 20 synthetic sample points if the file is missing.
 *  Returns 1 on success, 0 on fatal parse error. */
int parse_csv(void);

/** Access the live points array (read-only from other modules). */
ModbusPoint *data_get_points(void);

/** Number of configured points. */
int data_get_count(void);

/** Read a single point via Modbus RTU (with retries).
 *  Returns 1 on success and updates pt->value / pt->valid. */
int read_point(ModbusPoint *pt);

/** Read ALL configured points sequentially (blocking). */
void read_all_points(void);

#endif /* DATA_H */
